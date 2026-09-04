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

TokenService::TokenService(HostId local_host, SharedBumpAllocator* allocator, void* region_base,
                           TokenQueueResolver queue_resolver, VisibilityMode mode)
    : local_host_(local_host), allocator_(allocator), region_base_(static_cast<std::byte*>(region_base)),
      queue_resolver_(std::move(queue_resolver)), mode_(mode) {}

Result<AllocationDescriptor*> TokenService::Descriptor(GlobalPointer object) const {
    if (allocator_ == nullptr || region_base_ == nullptr)
        return Status::FailedPrecondition("token service requires a shared allocator");
    const auto descriptor = allocator_->MutableDescriptor(object);
    if (!descriptor.ok())
        return descriptor.status();
    if (!descriptor.value()->token_owner.is_lock_free() || !descriptor.value()->version.is_lock_free() ||
        !descriptor.value()->token_epoch.is_lock_free())
        return Status::FailedPrecondition("token metadata requires lock-free shared atomics");
    return descriptor.value();
}

Status TokenService::PushWithBackpressure(HostId destination, QueueEnvelope envelope) {
    const auto queue = queue_resolver_(local_host_, destination);
    if (!queue.ok())
        return queue.status();
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

Status TokenService::RegisterAllocation(GlobalPointer object) {
    const auto descriptor = Descriptor(object);
    if (!descriptor.ok())
        return descriptor.status();
    if (descriptor.value()->token_owner.load(std::memory_order_acquire) != local_host_)
        return Status::FailedPrecondition("new allocation token is not owned by the allocating host");
    std::lock_guard<std::mutex> lock(mutex_);
    objects_[object.offset].available = true;
    return Status::Ok();
}

Result<TokenRequestHandle> TokenService::Request(GlobalPointer object) {
    const auto descriptor = Descriptor(object);
    if (!descriptor.ok())
        return descriptor.status();
    const auto generation = descriptor.value()->generation;
    const auto request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    auto waiter = std::make_shared<Waiter>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters_.emplace(request_id, waiter);
    }

    while (true) {
        const auto owner = static_cast<HostId>(descriptor.value()->token_owner.load(std::memory_order_acquire));
        TokenRequest request;
        request.header = {MessageKind::kTokenReq, local_host_, owner, static_cast<std::uint32_t>(sizeof(request))};
        request.object = object;
        request.generation = generation;
        request.request_id = request_id;
        request.observed_version = descriptor.value()->version.load(std::memory_order_acquire);
        request.requester = local_host_;

        if (owner != local_host_) {
            const auto status = SendRequest(owner, request);
            if (!status.ok()) {
                std::lock_guard<std::mutex> lock(mutex_);
                waiters_.erase(request_id);
                return status;
            }
            return TokenRequestHandle {object, generation, request_id};
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (descriptor.value()->token_owner.load(std::memory_order_acquire) != local_host_)
            continue;
        auto& state = objects_[object.offset];
        if (!state.available || state.held) {
            state.pending.push_back(request);
        } else {
            state.held = true;
            waiter->lease = {object,
                             generation,
                             descriptor.value()->version.load(std::memory_order_acquire),
                             descriptor.value()->token_epoch.load(std::memory_order_acquire)};
            waiter->granted = true;
            waiter->ready.notify_one();
        }
        return TokenRequestHandle {object, generation, request_id};
    }
}

Result<TokenLease> TokenService::Wait(const TokenRequestHandle& request, std::uint64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = waiters_.find(request.request_id);
    if (found == waiters_.end())
        return Status::NotFound("unknown token request handle");
    const auto waiter = found->second;
    if (!waiter->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return waiter->granted; }))
        return Status::Unavailable("timed out waiting for token grant; request remains active");
    const auto lease = waiter->lease;
    waiters_.erase(found);
    return lease;
}

Status TokenService::Grant(const TokenRequest& request, AllocationDescriptor* descriptor) {
    if (request.requester == local_host_) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = waiters_.find(request.request_id);
        if (found == waiters_.end())
            return Status::NotFound("local token requester no longer exists");
        objects_[request.object.offset].held = true;
        objects_[request.object.offset].available = true;
        found->second->lease = {request.object,
                                request.generation,
                                descriptor->version.load(std::memory_order_acquire),
                                descriptor->token_epoch.load(std::memory_order_acquire)};
        found->second->granted = true;
        found->second->ready.notify_one();
        return Status::Ok();
    }

    const auto epoch = descriptor->token_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    descriptor->token_owner.store(request.requester, std::memory_order_release);
    const auto metadata_status = PublishData(descriptor, sizeof(*descriptor), mode_);
    if (!metadata_status.ok())
        return metadata_status;

    TokenGrant grant;
    grant.header = {MessageKind::kTokenGrant, local_host_, request.requester,
                    static_cast<std::uint32_t>(sizeof(grant))};
    grant.object = request.object;
    grant.generation = request.generation;
    grant.request_id = request.request_id;
    grant.new_owner = request.requester;
    grant.version = descriptor->version.load(std::memory_order_acquire);
    grant.token_epoch = epoch;
    const auto status = PushWithBackpressure(request.requester, Encode(grant));
    if (status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        objects_[request.object.offset].held = false;
        objects_[request.object.offset].available = false;
    }
    return status;
}

Status TokenService::HandleRequest(const TokenRequest& request) {
    if (request.requester >= kMaxHosts || request.generation == 0 || request.request_id == 0)
        return Status::InvalidArgument("invalid token request identity");
    const auto descriptor = Descriptor(request.object);
    if (!descriptor.ok())
        return descriptor.status();
    if (descriptor.value()->generation != request.generation)
        return Status::FailedPrecondition("token request targets a stale allocation generation");
    if (request.observed_version > descriptor.value()->version.load(std::memory_order_acquire))
        return Status::FailedPrecondition("token request observes a future object version");

    const auto owner = static_cast<HostId>(descriptor.value()->token_owner.load(std::memory_order_acquire));
    if (owner != local_host_)
        return SendRequest(owner, request);

    HostId forwarded_owner = local_host_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        forwarded_owner =
            static_cast<HostId>(descriptor.value()->token_owner.load(std::memory_order_acquire));
        if (forwarded_owner == local_host_) {
            auto& state = objects_[request.object.offset];
            if (!state.available || state.held) {
                state.pending.push_back(request);
                return Status::Ok();
            }
            state.held = true;
        }
    }
    if (forwarded_owner != local_host_)
        return SendRequest(forwarded_owner, request);
    return Grant(request, descriptor.value());
}

Status TokenService::HandleGrant(const TokenGrant& grant) {
    if (grant.new_owner != local_host_ || grant.generation == 0 || grant.request_id == 0)
        return Status::InvalidArgument("token grant is addressed to the wrong owner");
    const auto descriptor = Descriptor(grant.object);
    if (!descriptor.ok())
        return descriptor.status();
    const auto acquire_status = AcquireData(descriptor.value(), sizeof(*descriptor.value()), mode_);
    if (!acquire_status.ok())
        return acquire_status;
    if (descriptor.value()->generation != grant.generation ||
        descriptor.value()->token_owner.load(std::memory_order_acquire) != local_host_ ||
        descriptor.value()->token_epoch.load(std::memory_order_acquire) != grant.token_epoch ||
        descriptor.value()->version.load(std::memory_order_acquire) != grant.version)
        return Status::FailedPrecondition("token grant does not match authoritative metadata");

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = waiters_.find(grant.request_id);
    if (found == waiters_.end())
        return Status::NotFound("token grant has no matching request");
    objects_[grant.object.offset].held = true;
    objects_[grant.object.offset].available = true;
    found->second->lease = {grant.object, grant.generation, grant.version, grant.token_epoch};
    found->second->granted = true;
    found->second->ready.notify_one();
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
    return Status::InvalidArgument("message is not handled by the token service");
}

Status TokenService::Release(const TokenLease& lease) {
    const auto descriptor = Descriptor(lease.object);
    if (!descriptor.ok())
        return descriptor.status();
    if (descriptor.value()->generation != lease.generation ||
        descriptor.value()->token_owner.load(std::memory_order_acquire) != local_host_ ||
        descriptor.value()->token_epoch.load(std::memory_order_acquire) != lease.token_epoch)
        return Status::FailedPrecondition("token lease is stale or not locally owned");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = objects_.find(lease.object.offset);
        if (found == objects_.end() || !found->second.held)
            return Status::FailedPrecondition("token lease is not active in the local service");
    }

    const auto data_status = PublishData(region_base_ + lease.object.offset, descriptor.value()->bytes, mode_);
    if (!data_status.ok())
        return data_status;
    descriptor.value()->version.fetch_add(1, std::memory_order_acq_rel);
    const auto metadata_status = PublishData(descriptor.value(), sizeof(*descriptor.value()), mode_);
    if (!metadata_status.ok())
        return metadata_status;

    TokenRequest next;
    bool has_next = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = objects_[lease.object.offset];
        if (!state.pending.empty()) {
            next = state.pending.front();
            state.pending.pop_front();
            has_next = true;
        } else {
            state.held = false;
        }
    }
    return has_next ? Grant(next, descriptor.value()) : Status::Ok();
}

}  // namespace cxloom::loommem
