#include "cxloom/loommem/token.h"

#include <cstring>
#include <thread>
#include <type_traits>

namespace cxloom::loommem {
namespace {

template <typename T>
QueueEnvelope Encode(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) <= kQueuePayloadBytes);
    QueueEnvelope envelope;
    envelope.header = value.header;
    envelope.header.payload_bytes = sizeof(T);
    envelope.payload.resize(sizeof(T));
    std::memcpy(envelope.payload.data(), &value, sizeof(T));
    return envelope;
}

template <typename T>
Result<T> Decode(const QueueEnvelope& envelope, MessageKind kind) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (envelope.header.kind != kind || envelope.header.payload_bytes != sizeof(T) ||
        envelope.payload.size() != sizeof(T))
        return Status::InvalidArgument("token message has an invalid kind or payload size");
    T value {};
    std::memcpy(&value, envelope.payload.data(), sizeof(T));
    if (value.header.kind != envelope.header.kind || value.header.src_host != envelope.header.src_host ||
        value.header.dst_host != envelope.header.dst_host)
        return Status::InvalidArgument("token payload header does not match its queue envelope");
    return value;
}

}  // namespace

TokenService::TokenService(HostId local_host, SharedExtentAllocator* allocator, void* region_base,
                           TokenQueueResolver queue_resolver, VisibilityMode mode)
    : local_host_(local_host), allocator_(allocator), region_base_(static_cast<std::byte*>(region_base)),
      queue_resolver_(std::move(queue_resolver)), mode_(mode) {}

Result<TokenService::CoherenceTarget> TokenService::Target(GlobalPointer object, std::uint64_t block_index,
                                                            bool allow_retiring) const {
    if (allocator_ == nullptr || region_base_ == nullptr)
        return Status::FailedPrecondition("token service requires a shared allocator");
    const auto allocation = allocator_->MutableDescriptor(object, allow_retiring);
    if (!allocation.ok())
        return allocation.status();
    const auto block = allocator_->MutableCoherenceBlock(object, block_index, allow_retiring);
    if (!block.ok())
        return block.status();
    if (!block.value()->token_owner.is_lock_free() || !block.value()->version.is_lock_free() ||
        !block.value()->token_epoch.is_lock_free() || !block.value()->writeback_epoch.is_lock_free())
        return Status::FailedPrecondition("token metadata requires lock-free shared atomics");
    return CoherenceTarget {allocation.value(), block.value(),
                            allocation.value()->coherence_metadata_offset +
                                block_index * sizeof(CoherenceBlockDescriptor)};
}

Status TokenService::PushWithBackpressure(HostId destination, QueueEnvelope envelope) {
    if (destination >= kMaxHosts)
        return Status::InvalidArgument("token destination is out of range");
    const auto queue = queue_resolver_(local_host_, destination);
    if (!queue.ok())
        return queue.status();
    std::lock_guard<std::mutex> outbound_lock(outbound_mutexes_[destination]);
    while (true) {
        const auto status = queue.value()->Push(envelope);
        if (status.ok())
            return status;
        if (status.code() != StatusCode::kUnavailable)
            return status;
        std::this_thread::yield();
    }
}

Status TokenService::SendRequest(HostId destination, const TokenRequest& request) {
    auto forwarded = request;
    forwarded.header.src_host = local_host_;
    forwarded.header.dst_host = destination;
    forwarded.header.payload_bytes = sizeof(forwarded);
    return PushWithBackpressure(destination, Encode(forwarded));
}

Status TokenService::SendCancel(const TokenCancel& cancel) {
    if (cancel.header.dst_host == local_host_)
        return HandleCancel(cancel);
    return PushWithBackpressure(cancel.header.dst_host, Encode(cancel));
}

Status TokenService::Reject(const TokenRequest& request, TokenCompletionReason reason) {
    TokenReject reject;
    reject.header = {MessageKind::kTokenReject, local_host_, request.requester,
                     static_cast<std::uint32_t>(sizeof(reject))};
    reject.object = request.object;
    reject.block_index = request.block_index;
    reject.allocation_id = request.allocation_id;
    reject.request_id = request.request_id;
    reject.requester = request.requester;
    reject.reason = reason;
    return request.requester == local_host_ ? HandleReject(reject)
                                            : PushWithBackpressure(request.requester, Encode(reject));
}

Status TokenService::AcknowledgeCancel(const TokenCancel& cancel, TokenCompletionReason reason) {
    TokenCancelAck ack;
    ack.header = {MessageKind::kTokenCancelAck, local_host_, cancel.requester,
                  static_cast<std::uint32_t>(sizeof(ack))};
    ack.object = cancel.object;
    ack.block_index = cancel.block_index;
    ack.allocation_id = cancel.allocation_id;
    ack.request_id = cancel.request_id;
    ack.requester = cancel.requester;
    ack.reason = reason;
    return cancel.requester == local_host_ ? HandleCancelAck(ack)
                                           : PushWithBackpressure(cancel.requester, Encode(ack));
}

Status TokenService::RegisterAllocation(GlobalPointer object) {
    const auto allocation = allocator_->MutableDescriptor(object);
    if (!allocation.ok())
        return allocation.status();
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::uint64_t index = 0; index < allocation.value()->coherence_block_count; ++index) {
        const auto target = Target(object, index);
        if (!target.ok())
            return target.status();
        if (target.value().block->token_owner.load(std::memory_order_acquire) != local_host_)
            return Status::FailedPrecondition("new allocation block token is not owned by the allocating host");
        objects_[target.value().local_key].available = true;
    }
    return Status::Ok();
}

Status TokenService::ActivateWriter(CoherenceBlockDescriptor* block) {
    auto epoch = block->writeback_epoch.load(std::memory_order_acquire);
    while ((epoch & 1U) == 0) {
        if (block->writeback_epoch.compare_exchange_weak(epoch, epoch + 1, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
            const auto status = PublishData(block, sizeof(*block), mode_);
            if (!status.ok()) {
                block->writeback_epoch.store(epoch, std::memory_order_release);
                return status;
            }
            return Status::Ok();
        }
    }
    return Status::FailedPrecondition("another writer is already active for this object");
}

Result<TokenRequestHandle> TokenService::Request(GlobalPointer object, std::uint64_t block_index,
                                                 bool activate_coherence_epoch) {
    const auto target = Target(object, block_index);
    if (!target.ok())
        return target.status();
    const auto allocation_id = target.value().allocation->allocation_id;
    const auto request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    auto waiter = std::make_shared<Waiter>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters_.emplace(request_id, waiter);
    }

    while (true) {
        const auto owner = static_cast<HostId>(target.value().block->token_owner.load(std::memory_order_acquire));
        TokenRequest request;
        request.header = {MessageKind::kTokenReq, local_host_, owner, static_cast<std::uint32_t>(sizeof(request))};
        request.object = object;
        request.block_index = block_index;
        request.allocation_id = allocation_id;
        request.request_id = request_id;
        request.observed_version = target.value().block->version.load(std::memory_order_acquire);
        request.requester = local_host_;
        request.activate_coherence_epoch = activate_coherence_epoch;

        if (owner != local_host_) {
            const auto status = SendRequest(owner, request);
            if (!status.ok()) {
                std::lock_guard<std::mutex> lock(mutex_);
                waiters_.erase(request_id);
                return status;
            }
                return TokenRequestHandle {object, block_index, allocation_id, request_id};
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (target.value().allocation->state.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(AllocationState::kAllocated)) {
            waiters_.erase(request_id);
            return Status::FailedPrecondition("token request targets a retiring allocation");
        }
        if (target.value().block->token_owner.load(std::memory_order_acquire) != local_host_)
            continue;
        auto& state = objects_[target.value().local_key];
        if (!state.available || state.held) {
            state.pending.push_back(request);
        } else {
            state.held = true;
            if (activate_coherence_epoch) {
                const auto activate_status = ActivateWriter(target.value().block);
                if (!activate_status.ok()) {
                    state.held = false;
                    waiters_.erase(request_id);
                    return activate_status;
                }
            }
            waiter->lease = {object, block_index, allocation_id,
                             target.value().block->version.load(std::memory_order_acquire),
                             target.value().block->token_epoch.load(std::memory_order_acquire)};
            waiter->granted = true;
            waiter->state = Waiter::State::kGranted;
            waiter->ready.notify_one();
        }
        return TokenRequestHandle {object, block_index, allocation_id, request_id};
    }
}

Result<TokenLease> TokenService::Wait(const TokenRequestHandle& request, std::uint64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = waiters_.find(request.request_id);
    if (found == waiters_.end())
        return Status::NotFound("unknown token request handle");
    const auto waiter = found->second;
    if (!waiter->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return waiter->state == Waiter::State::kGranted || waiter->state == Waiter::State::kRejected ||
                   waiter->state == Waiter::State::kCancelled;
        }))
        return Status::Unavailable("timed out waiting for token grant; request remains active");
    if (waiter->state != Waiter::State::kGranted) {
        const auto status = waiter->completion_status;
        waiters_.erase(found);
        return status;
    }
    const auto lease = waiter->lease;
    waiters_.erase(found);
    return lease;
}

Status TokenService::Cancel(const TokenRequestHandle& request) {
    TokenLease granted_lease;
    bool release_grant = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = waiters_.find(request.request_id);
        if (found == waiters_.end())
            return Status::NotFound("unknown token request handle");
        if (found->second->state != Waiter::State::kGranted) {
            if (found->second->state == Waiter::State::kRejected ||
                found->second->state == Waiter::State::kCancelled) {
                const auto status = found->second->completion_status;
                waiters_.erase(found);
                return status;
            }
            found->second->abandoned = true;
            found->second->state = Waiter::State::kCancelling;
        } else {
            granted_lease = found->second->lease;
            waiters_.erase(found);
            release_grant = true;
        }
    }
    if (release_grant)
        return Release(granted_lease, false);
    TokenCancel cancel;
    cancel.object = request.object;
    cancel.block_index = request.block_index;
    cancel.allocation_id = request.allocation_id;
    cancel.request_id = request.request_id;
    cancel.requester = local_host_;
    const auto target = Target(request.object, request.block_index, true);
    if (!target.ok() || target.value().allocation->allocation_id != request.allocation_id) {
        cancel.header = {MessageKind::kTokenCancel, local_host_, local_host_,
                         static_cast<std::uint32_t>(sizeof(cancel))};
        return AcknowledgeCancel(cancel, TokenCompletionReason::kCancelled);
    }
    const auto owner = static_cast<HostId>(target.value().block->token_owner.load(std::memory_order_acquire));
    cancel.header = {MessageKind::kTokenCancel, local_host_, owner,
                     static_cast<std::uint32_t>(sizeof(cancel))};
    return SendCancel(cancel);
}

Status TokenService::CancelAndWait(const TokenRequestHandle& request, std::uint64_t timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = waiters_.find(request.request_id);
        if (found == waiters_.end())
            return Status::NotFound("unknown token request handle");
        found->second->retain_completion = true;
    }
    const auto cancel_status = Cancel(request);
    if (!cancel_status.ok())
        return cancel_status;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = waiters_.find(request.request_id);
    if (found == waiters_.end())
        return Status::Ok();
    const auto waiter = found->second;
    if (!waiter->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return waiter->state == Waiter::State::kCancelled || waiter->state == Waiter::State::kRejected ||
                   waiter->state == Waiter::State::kGranted;
        }))
        return Status::Unavailable("timed out waiting for token cancellation completion");
    if (waiter->state == Waiter::State::kGranted) {
        const auto lease = waiter->lease;
        waiters_.erase(found);
        lock.unlock();
        return Release(lease, false);
    }
    waiters_.erase(found);
    return Status::Ok();
}

Status TokenService::Grant(const TokenRequest& request, const CoherenceTarget& target) {
    if (target.allocation->state.load(std::memory_order_acquire) !=
        static_cast<std::uint32_t>(AllocationState::kAllocated))
        return Status::FailedPrecondition("cannot grant a token for a retiring allocation");
    if (request.requester == local_host_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (waiters_.find(request.request_id) == waiters_.end())
                return Status::NotFound("local token requester no longer exists");
        }
        if (request.activate_coherence_epoch) {
            const auto activate_status = ActivateWriter(target.block);
            if (!activate_status.ok())
                return activate_status;
        }
        TokenLease lease {request.object, request.block_index, request.allocation_id,
                          target.block->version.load(std::memory_order_acquire),
                          target.block->token_epoch.load(std::memory_order_acquire)};
        bool abandoned = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = waiters_.find(request.request_id);
            if (found == waiters_.end())
                return Status::NotFound("local token requester disappeared during grant");
            objects_[target.local_key].held = true;
            objects_[target.local_key].available = true;
            abandoned = found->second->abandoned;
            if (abandoned) {
                found->second->state = Waiter::State::kCancelled;
                found->second->completion_status = Status::FailedPrecondition("late token grant was cancelled");
                found->second->ready.notify_all();
                if (!found->second->retain_completion)
                    waiters_.erase(found);
            } else {
                found->second->lease = lease;
                found->second->granted = true;
                found->second->state = Waiter::State::kGranted;
                found->second->ready.notify_one();
            }
        }
        return abandoned ? Release(lease, false) : Status::Ok();
    }

    const auto epoch = target.block->token_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    target.block->token_owner.store(request.requester, std::memory_order_release);
    const auto metadata_status = PublishData(target.block, sizeof(*target.block), mode_);
    if (!metadata_status.ok())
        return metadata_status;

    TokenGrant grant;
    grant.header = {MessageKind::kTokenGrant, local_host_, request.requester,
                    static_cast<std::uint32_t>(sizeof(grant))};
    grant.object = request.object;
    grant.block_index = request.block_index;
    grant.allocation_id = request.allocation_id;
    grant.request_id = request.request_id;
    grant.new_owner = request.requester;
    grant.version = target.block->version.load(std::memory_order_acquire);
    grant.token_epoch = epoch;
    grant.activate_coherence_epoch = request.activate_coherence_epoch;
    const auto status = PushWithBackpressure(request.requester, Encode(grant));
    if (status.ok()) {
        std::deque<TokenRequest> remaining;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = objects_[target.local_key];
            state.held = false;
            state.available = false;
            remaining.swap(state.pending);
        }
        for (const auto& pending : remaining) {
            const auto forward_status = SendRequest(request.requester, pending);
            if (!forward_status.ok())
                return forward_status;
        }
    }
    return status;
}

Status TokenService::HandleRequest(const TokenRequest& request) {
    if (request.requester >= kMaxHosts || request.allocation_id == 0 || request.request_id == 0)
        return Status::InvalidArgument("invalid token request identity");
    const auto target = Target(request.object, request.block_index);
    if (!target.ok())
        return Reject(request, target.status().code() == StatusCode::kInvalidArgument
                                   ? TokenCompletionReason::kInvalidBlock
                                   : TokenCompletionReason::kInvalidMetadata);
    if (target.value().allocation->allocation_id != request.allocation_id)
        return Reject(request, TokenCompletionReason::kStaleAllocation);
    if (target.value().allocation->state.load(std::memory_order_acquire) !=
        static_cast<std::uint32_t>(AllocationState::kAllocated))
        return Reject(request, TokenCompletionReason::kRetiring);
    if (request.observed_version > target.value().block->version.load(std::memory_order_acquire))
        return Reject(request, TokenCompletionReason::kInvalidMetadata);

    const auto owner = static_cast<HostId>(target.value().block->token_owner.load(std::memory_order_acquire));
    if (owner != local_host_)
        return SendRequest(owner, request);

    HostId forwarded_owner = local_host_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (target.value().allocation->state.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(AllocationState::kAllocated)) {
            forwarded_owner = kMaxHosts;
        } else {
        forwarded_owner =
            static_cast<HostId>(target.value().block->token_owner.load(std::memory_order_acquire));
        if (forwarded_owner == local_host_) {
            auto& state = objects_[target.value().local_key];
            if (!state.available || state.held) {
                state.pending.push_back(request);
                return Status::Ok();
            }
            state.held = true;
        }
        }
    }
    if (forwarded_owner == kMaxHosts)
        return Reject(request, TokenCompletionReason::kRetiring);
    if (forwarded_owner != local_host_)
        return SendRequest(forwarded_owner, request);
    return Grant(request, target.value());
}

Status TokenService::HandleGrant(const TokenGrant& grant) {
    if (grant.new_owner != local_host_ || grant.allocation_id == 0 || grant.request_id == 0)
        return Status::InvalidArgument("token grant is addressed to the wrong owner");
    const auto target = Target(grant.object, grant.block_index);
    if (!target.ok()) {
        TokenReject reject {{MessageKind::kTokenReject, local_host_, local_host_, sizeof(TokenReject)},
                            grant.object, grant.block_index, grant.allocation_id, grant.request_id,
                            local_host_, TokenCompletionReason::kRetiring};
        return HandleReject(reject);
    }
    const auto acquire_status = AcquireData(target.value().block, sizeof(*target.value().block), mode_);
    if (!acquire_status.ok())
        return acquire_status;
    if (target.value().allocation->allocation_id != grant.allocation_id ||
        target.value().block->token_owner.load(std::memory_order_acquire) != local_host_ ||
        target.value().block->token_epoch.load(std::memory_order_acquire) != grant.token_epoch ||
        target.value().block->version.load(std::memory_order_acquire) != grant.version)
        return Status::FailedPrecondition("token grant does not match authoritative metadata");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (waiters_.find(grant.request_id) == waiters_.end())
            return Status::NotFound("token grant has no matching request");
    }
    if (grant.activate_coherence_epoch) {
        const auto activate_status = ActivateWriter(target.value().block);
        if (!activate_status.ok())
            return activate_status;
    }
    const TokenLease lease {grant.object, grant.block_index, grant.allocation_id, grant.version, grant.token_epoch};
    bool abandoned = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = waiters_.find(grant.request_id);
        if (found == waiters_.end())
            return Status::NotFound("token requester disappeared during grant");
        objects_[target.value().local_key].held = true;
        objects_[target.value().local_key].available = true;
        abandoned = found->second->abandoned;
        if (abandoned) {
            found->second->state = Waiter::State::kCancelled;
            found->second->completion_status = Status::FailedPrecondition("late token grant was cancelled");
            found->second->ready.notify_all();
            if (!found->second->retain_completion)
                waiters_.erase(found);
        } else {
            found->second->lease = lease;
            found->second->granted = true;
            found->second->state = Waiter::State::kGranted;
            found->second->ready.notify_one();
        }
    }
    return abandoned ? Release(lease, false) : Status::Ok();
}

Status TokenService::HandleReject(const TokenReject& reject) {
    if (reject.requester != local_host_ || reject.request_id == 0)
        return Status::InvalidArgument("token rejection is addressed to the wrong requester");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = waiters_.find(reject.request_id);
    if (found == waiters_.end())
        return Status::Ok();
    auto& waiter = *found->second;
    if (waiter.state == Waiter::State::kGranted)
        return Status::Ok();
    const bool cancelling = waiter.state == Waiter::State::kCancelling;
    waiter.state = cancelling ? Waiter::State::kCancelled : Waiter::State::kRejected;
    waiter.completion_status = Status::FailedPrecondition("token request was rejected by its owner");
    waiter.ready.notify_all();
    if (waiter.abandoned && !waiter.retain_completion)
        waiters_.erase(found);
    return Status::Ok();
}

Status TokenService::HandleCancel(const TokenCancel& cancel) {
    if (cancel.requester >= kMaxHosts || cancel.request_id == 0)
        return Status::InvalidArgument("invalid token cancellation identity");
    const auto target = Target(cancel.object, cancel.block_index, true);
    if (!target.ok() || target.value().allocation->allocation_id != cancel.allocation_id)
        return AcknowledgeCancel(cancel, TokenCompletionReason::kCancelled);
    const auto owner = static_cast<HostId>(target.value().block->token_owner.load(std::memory_order_acquire));
    if (owner != local_host_) {
        auto forwarded = cancel;
        forwarded.header.src_host = local_host_;
        forwarded.header.dst_host = owner;
        return SendCancel(forwarded);
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& pending = objects_[target.value().local_key].pending;
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->requester == cancel.requester && it->request_id == cancel.request_id &&
                it->allocation_id == cancel.allocation_id && it->block_index == cancel.block_index) {
                pending.erase(it);
                removed = true;
                break;
            }
        }
    }
    const auto reason = removed || owner != cancel.requester ? TokenCompletionReason::kCancelled
                                                             : TokenCompletionReason::kTooLate;
    return AcknowledgeCancel(cancel, reason);
}

Status TokenService::HandleCancelAck(const TokenCancelAck& ack) {
    if (ack.requester != local_host_ || ack.request_id == 0)
        return Status::InvalidArgument("token cancellation ack is addressed to the wrong requester");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = waiters_.find(ack.request_id);
    if (found == waiters_.end())
        return Status::Ok();
    if (ack.reason == TokenCompletionReason::kTooLate)
        return Status::Ok();
    found->second->state = Waiter::State::kCancelled;
    found->second->completion_status = Status::FailedPrecondition("token request was cancelled");
    found->second->ready.notify_all();
    if (found->second->abandoned && !found->second->retain_completion)
        waiters_.erase(found);
    return Status::Ok();
}

Status TokenService::HandleRetire(const TokenRetire& retire) {
    if (retire.coordinator >= kMaxHosts || retire.allocation_id == 0 || retire.retirement_id == 0)
        return Status::InvalidArgument("invalid token retirement identity");
    const auto target = Target(retire.object, retire.block_index, true);
    if (!target.ok() || target.value().allocation->allocation_id != retire.allocation_id)
        return Status::FailedPrecondition("token retirement targets a stale allocation");
    const auto owner = static_cast<HostId>(target.value().block->token_owner.load(std::memory_order_acquire));
    if (owner != local_host_) {
        auto forwarded = retire;
        forwarded.header.src_host = local_host_;
        forwarded.header.dst_host = owner;
        return PushWithBackpressure(owner, Encode(forwarded));
    }

    std::deque<TokenRequest> rejected;
    if (target.value().allocation->state.load(std::memory_order_acquire) ==
        static_cast<std::uint32_t>(AllocationState::kRetiring)) {
        std::lock_guard<std::mutex> lock(mutex_);
        rejected.swap(objects_[target.value().local_key].pending);
    }
    for (const auto& request : rejected) {
        const auto status = Reject(request, TokenCompletionReason::kRetiring);
        if (!status.ok())
            return status;
    }

    TokenRetireAck ack;
    ack.header = {MessageKind::kTokenRetireAck, local_host_, retire.coordinator,
                  static_cast<std::uint32_t>(sizeof(ack))};
    ack.object = retire.object;
    ack.block_index = retire.block_index;
    ack.allocation_id = retire.allocation_id;
    ack.retirement_id = retire.retirement_id;
    ack.coordinator = retire.coordinator;
    return retire.coordinator == local_host_ ? HandleRetireAck(ack)
                                             : PushWithBackpressure(retire.coordinator, Encode(ack));
}

Status TokenService::HandleRetireAck(const TokenRetireAck& ack) {
    if (ack.coordinator != local_host_ || ack.retirement_id == 0)
        return Status::InvalidArgument("token retirement acknowledgment is addressed to the wrong coordinator");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = retirement_waiters_.find(ack.retirement_id);
    if (found == retirement_waiters_.end())
        return Status::Ok();
    auto& waiter = *found->second;
    if (waiter.object.region_id != ack.object.region_id || waiter.object.offset != ack.object.offset ||
        waiter.allocation_id != ack.allocation_id)
        return Status::FailedPrecondition("token retirement acknowledgment identity does not match");
    if (waiter.acknowledged_blocks.insert(ack.block_index).second && waiter.remaining != 0)
        --waiter.remaining;
    if (waiter.remaining == 0)
        waiter.ready.notify_all();
    return Status::Ok();
}

Status TokenService::HandleMessage(const QueueEnvelope& envelope) {
    if (envelope.header.kind == MessageKind::kTokenReq) {
        const auto request = Decode<TokenRequest>(envelope, MessageKind::kTokenReq);
        return request.ok() ? HandleRequest(request.value()) : request.status();
    }
    if (envelope.header.kind == MessageKind::kTokenGrant) {
        const auto grant = Decode<TokenGrant>(envelope, MessageKind::kTokenGrant);
        return grant.ok() ? HandleGrant(grant.value()) : grant.status();
    }
    if (envelope.header.kind == MessageKind::kTokenReject) {
        const auto reject = Decode<TokenReject>(envelope, MessageKind::kTokenReject);
        return reject.ok() ? HandleReject(reject.value()) : reject.status();
    }
    if (envelope.header.kind == MessageKind::kTokenCancel) {
        const auto cancel = Decode<TokenCancel>(envelope, MessageKind::kTokenCancel);
        return cancel.ok() ? HandleCancel(cancel.value()) : cancel.status();
    }
    if (envelope.header.kind == MessageKind::kTokenCancelAck) {
        const auto ack = Decode<TokenCancelAck>(envelope, MessageKind::kTokenCancelAck);
        return ack.ok() ? HandleCancelAck(ack.value()) : ack.status();
    }
    if (envelope.header.kind == MessageKind::kTokenRetire) {
        const auto retire = Decode<TokenRetire>(envelope, MessageKind::kTokenRetire);
        return retire.ok() ? HandleRetire(retire.value()) : retire.status();
    }
    if (envelope.header.kind == MessageKind::kTokenRetireAck) {
        const auto ack = Decode<TokenRetireAck>(envelope, MessageKind::kTokenRetireAck);
        return ack.ok() ? HandleRetireAck(ack.value()) : ack.status();
    }
    return Status::InvalidArgument("message is not handled by the token service");
}

Status TokenService::BeginWriteback(const TokenLease& lease) {
    const auto target = Target(lease.object, lease.block_index, true);
    if (!target.ok())
        return target.status();
    if (target.value().allocation->allocation_id != lease.allocation_id ||
        target.value().block->token_owner.load(std::memory_order_acquire) != local_host_ ||
        target.value().block->token_epoch.load(std::memory_order_acquire) != lease.token_epoch)
        return Status::FailedPrecondition("token lease is stale or not locally owned");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = objects_.find(target.value().local_key);
        if (found == objects_.end() || !found->second.held)
            return Status::FailedPrecondition("token lease is not active in the local service");
    }
    return ActivateWriter(target.value().block);
}

Status TokenService::Release(const TokenLease& lease, bool modified) {
    const auto target = Target(lease.object, lease.block_index, true);
    if (!target.ok())
        return target.status();
    if (target.value().allocation->allocation_id != lease.allocation_id ||
        target.value().block->token_owner.load(std::memory_order_acquire) != local_host_ ||
        target.value().block->token_epoch.load(std::memory_order_acquire) != lease.token_epoch)
        return Status::FailedPrecondition("token lease is stale or not locally owned");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = objects_.find(target.value().local_key);
        if (found == objects_.end() || !found->second.held)
            return Status::FailedPrecondition("token lease is not active in the local service");
    }

    const auto coherence_epoch = target.value().block->writeback_epoch.load(std::memory_order_acquire);
    if (modified && (coherence_epoch & 1U) == 0)
        return Status::FailedPrecondition("modified release requires an active writeback epoch");
    if (modified) {
        const auto block_offset = lease.block_index * target.value().allocation->coherence_block_bytes;
        const auto block_bytes = std::min(target.value().allocation->coherence_block_bytes,
                                          target.value().allocation->bytes - block_offset);
        const auto data_status = PublishData(region_base_ + lease.object.offset + block_offset, block_bytes, mode_);
        if (!data_status.ok())
            return data_status;
        target.value().block->version.fetch_add(1, std::memory_order_acq_rel);
    }
    if ((coherence_epoch & 1U) != 0)
        target.value().block->writeback_epoch.store(coherence_epoch + 1, std::memory_order_release);
    const auto metadata_status = PublishData(target.value().block, sizeof(*target.value().block), mode_);
    if (!metadata_status.ok())
        return metadata_status;

    TokenRequest next;
    bool has_next = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = objects_[target.value().local_key];
        if (!state.pending.empty()) {
            next = state.pending.front();
            state.pending.pop_front();
            has_next = true;
        } else {
            state.held = false;
        }
    }
    return has_next ? Grant(next, target.value()) : Status::Ok();
}

Status TokenService::PrepareRetire(GlobalPointer object, std::uint64_t timeout_ms) {
    const auto allocation = allocator_->MutableDescriptor(object);
    if (!allocation.ok())
        return allocation.status();
    auto* descriptor = allocation.value();
    const auto allocation_id = descriptor->allocation_id;
    auto expected = static_cast<std::uint32_t>(AllocationState::kAllocated);
    if (!descriptor->state.compare_exchange_strong(
            expected, static_cast<std::uint32_t>(AllocationState::kRetiring),
            std::memory_order_acq_rel, std::memory_order_acquire))
        return Status::NotFound("allocation is already retiring or free");

    const auto retirement_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    auto waiter = std::make_shared<RetirementWaiter>();
    waiter->object = object;
    waiter->allocation_id = allocation_id;
    waiter->remaining = descriptor->coherence_block_count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retirement_waiters_.emplace(retirement_id, waiter);
    }
    for (std::uint64_t block_index = 0; block_index < descriptor->coherence_block_count; ++block_index) {
        const auto target = Target(object, block_index, true);
        if (!target.ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            retirement_waiters_.erase(retirement_id);
            descriptor->state.store(static_cast<std::uint32_t>(AllocationState::kAllocated),
                                    std::memory_order_release);
            return target.status();
        }
        const auto owner = static_cast<HostId>(target.value().block->token_owner.load(std::memory_order_acquire));
        TokenRetire retire;
        retire.header = {MessageKind::kTokenRetire, local_host_, owner,
                         static_cast<std::uint32_t>(sizeof(retire))};
        retire.object = object;
        retire.block_index = block_index;
        retire.allocation_id = allocation_id;
        retire.retirement_id = retirement_id;
        retire.coordinator = local_host_;
        const auto status = owner == local_host_ ? HandleRetire(retire)
                                                 : PushWithBackpressure(owner, Encode(retire));
        if (!status.ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            retirement_waiters_.erase(retirement_id);
            descriptor->state.store(static_cast<std::uint32_t>(AllocationState::kAllocated),
                                    std::memory_order_release);
            return status;
        }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!waiter->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [&] { return waiter->remaining == 0; })) {
        retirement_waiters_.erase(retirement_id);
        descriptor->state.store(static_cast<std::uint32_t>(AllocationState::kAllocated),
                                std::memory_order_release);
        return Status::Unavailable("timed out draining token requests for retiring allocation");
    }
    retirement_waiters_.erase(retirement_id);
    return Status::Ok();
}

void TokenService::ForgetAllocation(GlobalPointer, std::uint64_t coherence_metadata_offset,
                                    std::uint64_t block_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::uint64_t index = 0; index < block_count; ++index)
        objects_.erase(coherence_metadata_offset + index * sizeof(CoherenceBlockDescriptor));
}

std::size_t TokenService::pending_request_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

}  // namespace cxloom::loommem
