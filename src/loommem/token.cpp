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
    const auto allocation = allocator_->AcquireReference(object, local_host_, allow_retiring);
    if (!allocation.ok())
        return allocation.status();
    std::shared_ptr<void> reference(allocation.value(), [this](void* p) {
        allocator_->ReleaseReference(static_cast<AllocationDescriptor*>(p), local_host_);
    });
    const auto block = allocator_->MutableCoherenceBlock(object, block_index, true);
    if (!block.ok())
        return block.status();
    if (!block.value()->token_owner.is_lock_free() || !block.value()->version.is_lock_free() ||
        !block.value()->token_epoch.is_lock_free() || !block.value()->writeback_epoch.is_lock_free())
        return Status::FailedPrecondition("token metadata requires lock-free shared atomics");
    return CoherenceTarget {std::move(reference), allocation.value(), block.value(),
                            allocation.value()->coherence_metadata_offset +
                                block_index * sizeof(CoherenceBlockDescriptor)};
}

Status TokenService::EnqueueOutbound(HostId destination, QueueEnvelope envelope) {
    if (destination >= kMaxHosts)
        return Status::InvalidArgument("token destination is out of range");
    // Never block the poller on a full outbound queue. The protocol lock orders
    // producers, and progress flushes this FIFO before recording watermarks.
    outbound_[destination].push_back(std::move(envelope));
    return ProgressOutbound();
}

Status TokenService::SendRequest(HostId destination, const TokenRequest& request) {
    auto forwarded = request;
    forwarded.header.src_host = local_host_;
    forwarded.header.dst_host = destination;
    forwarded.header.payload_bytes = sizeof(forwarded);
    return EnqueueOutbound(destination, Encode(forwarded));
}

Status TokenService::SendCancel(const TokenCancel& cancel) {
    if (cancel.header.dst_host == local_host_)
        return HandleCancel(cancel);
    return EnqueueOutbound(cancel.header.dst_host, Encode(cancel));
}

Status TokenService::Reject(const TokenRequest& request, TokenCompletionReason reason) {
    TokenReject reject;
    reject.header = {MessageKind::kTokenReject, local_host_, request.requester,
                     static_cast<std::uint32_t>(sizeof(reject))};
    reject.object = request.object;
    reject.block_index = request.block_index;
    reject.request_id = request.request_id;
    reject.requester = request.requester;
    reject.reason = reason;
    return request.requester == local_host_ ? HandleReject(reject)
                                            : EnqueueOutbound(request.requester, Encode(reject));
}

Status TokenService::AcknowledgeCancel(const TokenCancel& cancel, TokenCompletionReason reason) {
    TokenCancelAck ack;
    ack.header = {MessageKind::kTokenCancelAck, local_host_, cancel.requester,
                  static_cast<std::uint32_t>(sizeof(ack))};
    ack.object = cancel.object;
    ack.block_index = cancel.block_index;
    ack.request_id = cancel.request_id;
    ack.requester = cancel.requester;
    ack.reason = reason;
    return cancel.requester == local_host_ ? HandleCancelAck(ack)
                                           : EnqueueOutbound(cancel.requester, Encode(ack));
}

Status TokenService::RegisterAllocation(GlobalPointer object) {
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
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
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
    const auto target = Target(object, block_index);
    if (!target.ok())
        return target.status();
    const auto request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    auto waiter = std::make_shared<Waiter>();
    waiter->object = object;
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
                return TokenRequestHandle {object, block_index, request_id};
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
            waiter->lease = {object, block_index,
                             target.value().block->version.load(std::memory_order_acquire),
                             target.value().block->token_epoch.load(std::memory_order_acquire)};
            waiter->granted = true;
            waiter->state = Waiter::State::kGranted;
            waiter->ready.notify_one();
        }
        return TokenRequestHandle {object, block_index, request_id};
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
        waiters_.erase(request.request_id);
        return status;
    }
    const auto lease = waiter->lease;
    waiters_.erase(request.request_id);
    return lease;
}

Status TokenService::Cancel(const TokenRequestHandle& request) {
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
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
                waiters_.erase(request.request_id);
                return status;
            }
            found->second->abandoned = true;
            found->second->state = Waiter::State::kCancelling;
        } else {
            granted_lease = found->second->lease;
            waiters_.erase(request.request_id);
            release_grant = true;
        }
    }
    if (release_grant)
        return Release(granted_lease, false);
    if (allocator_->IsRetiring(request.object)) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = waiters_.find(request.request_id);
        if (found != waiters_.end()) {
            found->second->state = Waiter::State::kCancelled;
            found->second->completion_status = Status::FailedPrecondition("object is retiring");
            found->second->ready.notify_all();
            waiters_.erase(request.request_id);
        }
        return Status::Ok();
    }
    TokenCancel cancel;
    cancel.object = request.object;
    cancel.block_index = request.block_index;
    cancel.request_id = request.request_id;
    cancel.requester = local_host_;
    const auto target = Target(request.object, request.block_index, true);
    if (!target.ok()) {
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
        waiters_.erase(request.request_id);
        lock.unlock();
        return Release(lease, false);
    }
    waiters_.erase(request.request_id);
    return Status::Ok();
}

Status TokenService::Grant(const TokenRequest& request, const CoherenceTarget& target) {
    if (target.allocation->state.load(std::memory_order_acquire) !=
        static_cast<std::uint32_t>(AllocationState::kAllocated)) {
        std::lock_guard<std::mutex> lock(mutex_);
        objects_[target.local_key].held = false;
        return Status::Ok();
    }
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
        TokenLease lease {request.object, request.block_index,
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
                    waiters_.erase(request.request_id);
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
    grant.request_id = request.request_id;
    grant.new_owner = request.requester;
    grant.version = target.block->version.load(std::memory_order_acquire);
    grant.token_epoch = epoch;
    grant.activate_coherence_epoch = request.activate_coherence_epoch;
    const auto status = EnqueueOutbound(request.requester, Encode(grant));
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
    if (request.requester >= kMaxHosts || request.request_id == 0)
        return Status::InvalidArgument("invalid token request identity");
    const auto target = Target(request.object, request.block_index);
    if (!target.ok())
        return Reject(request, target.status().code() == StatusCode::kInvalidArgument
                                   ? TokenCompletionReason::kInvalidBlock
                                   : TokenCompletionReason::kInvalidMetadata);
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
    if (grant.new_owner != local_host_ || grant.request_id == 0)
        return Status::InvalidArgument("token grant is addressed to the wrong owner");
    const auto target = Target(grant.object, grant.block_index);
    if (!target.ok()) {
        TokenReject reject {{MessageKind::kTokenReject, local_host_, local_host_, sizeof(TokenReject)},
                            grant.object, grant.block_index, grant.request_id,
                            local_host_, TokenCompletionReason::kRetiring};
        return HandleReject(reject);
    }
    const auto acquire_status = AcquireData(target.value().block, sizeof(*target.value().block), mode_);
    if (!acquire_status.ok())
        return acquire_status;
    if (target.value().block->token_owner.load(std::memory_order_acquire) != local_host_ ||
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
    const TokenLease lease {grant.object, grant.block_index, grant.version, grant.token_epoch};
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
                waiters_.erase(grant.request_id);
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
        waiters_.erase(reject.request_id);
    return Status::Ok();
}

Status TokenService::HandleCancel(const TokenCancel& cancel) {
    if (cancel.requester >= kMaxHosts || cancel.request_id == 0)
        return Status::InvalidArgument("invalid token cancellation identity");
    const auto target = Target(cancel.object, cancel.block_index, true);
    if (!target.ok())
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
                it->block_index == cancel.block_index) {
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
        waiters_.erase(ack.request_id);
    return Status::Ok();
}

Status TokenService::HandleMessage(const QueueEnvelope& envelope) {
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
    if (envelope.payload.size() < offsetof(TokenRequest, object) + sizeof(GlobalPointer))
        return Status::InvalidArgument("token envelope is too short");
    GlobalPointer object {};
    std::memcpy(&object, envelope.payload.data() + offsetof(TokenRequest, object), sizeof(object));
    if (allocator_->IsRetiring(object)) return Status::Ok();
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
    return Status::InvalidArgument("message is not handled by the token service");
}

Status TokenService::BeginWriteback(const TokenLease& lease) {
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
    const auto target = Target(lease.object, lease.block_index, true);
    if (!target.ok())
        return target.status();
    if (target.value().block->token_owner.load(std::memory_order_acquire) != local_host_ ||
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
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
    const auto target = Target(lease.object, lease.block_index, true);
    if (!target.ok())
        return target.status();
    if (target.value().block->token_owner.load(std::memory_order_acquire) != local_host_ ||
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
        target.value().block->last_writer.store(local_host_, std::memory_order_release);
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
        if (allocator_->IsRetiring(lease.object)) {
            state.pending.clear();
            state.held = false;
        } else if (!state.pending.empty()) {
            next = state.pending.front();
            state.pending.pop_front();
            has_next = true;
        } else {
            state.held = false;
        }
    }
    return has_next ? Grant(next, target.value()) : Status::Ok();
}

Status TokenService::ProgressOutbound() {
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
    for (HostId host = 0; host < kMaxHosts; ++host) {
        auto& pending = outbound_[host];
        if (pending.empty()) continue;
        const auto queue = queue_resolver_(local_host_, host);
        if (!queue.ok()) return queue.status();
        while (!pending.empty()) {
            const auto status = queue.value()->Push(pending.front());
            if (status.code() == StatusCode::kUnavailable) break;
            if (!status.ok()) return status;
            pending.pop_front();
        }
    }
    return Status::Ok();
}

bool TokenService::CloseObject(GlobalPointer object) {
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
    std::vector<TokenLease> unclaimed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            auto& waiter = *it->second;
            if (!(waiter.object == object)) { ++it; continue; }
            if (waiter.state == Waiter::State::kGranted) unclaimed.push_back(waiter.lease);
            waiter.state = Waiter::State::kRejected;
            waiter.completion_status = Status::FailedPrecondition("object is retiring");
            waiter.ready.notify_all();
            it = waiters_.erase(it);
        }
    }
    // Unclaimed grants have no application writer. Claimed leases remain held
    // until their caller completes; retirement never steals a live write buffer.
    for (const auto& lease : unclaimed) {
        if (!Release(lease, false).ok()) return false;
    }
    const auto descriptor = allocator_->MutableDescriptor(object, true);
    if (!descriptor.ok()) return false;
    bool held = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::uint64_t block = 0; block < descriptor.value()->coherence_block_count; ++block) {
            const auto key = descriptor.value()->coherence_metadata_offset + block * sizeof(CoherenceBlockDescriptor);
            const auto found = objects_.find(key);
            if (found == objects_.end()) continue;
            found->second.pending.clear();
            held = held || found->second.held;
        }
    }
    // All participants cancel locally, so unsent retiring-object packets can
    // be discarded. Already published packets are covered by FIFO watermarks.
    for (auto& pending : outbound_) {
        for (auto it = pending.begin(); it != pending.end();) {
            GlobalPointer target {};
            std::memcpy(&target, it->payload.data() + offsetof(TokenRequest, object), sizeof(target));
            if (target == object) it = pending.erase(it); else ++it;
        }
    }
    return !held && descriptor.value()->active_operations[local_host_].load(std::memory_order_acquire) == 0;
}

void TokenService::ForgetAllocation(GlobalPointer, std::uint64_t coherence_metadata_offset,
                                    std::uint64_t block_count) {
    std::lock_guard<std::recursive_mutex> protocol_lock(protocol_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::uint64_t index = 0; index < block_count; ++index)
        objects_.erase(coherence_metadata_offset + index * sizeof(CoherenceBlockDescriptor));
}

std::size_t TokenService::pending_request_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

}  // namespace cxloom::loommem
