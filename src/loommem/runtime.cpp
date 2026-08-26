#include "cxloom/loommem/runtime.h"

#include "cxloom/common/tracing.h"

namespace cxloom::loommem {

SharedRegionLayout BuildDefaultLayout(std::size_t total_bytes) {
    const std::uint64_t bootstrap_bytes = 1ULL << 20;
    const std::uint64_t allocator_bytes = 1ULL << 20;
    const std::uint64_t coherence_bytes = 64ULL << 20;
    const std::uint64_t queues_bytes = 64ULL << 20;
    const std::uint64_t shared_data_offset = bootstrap_bytes + allocator_bytes + coherence_bytes + queues_bytes;
    const std::uint64_t shared_data_bytes = total_bytes > shared_data_offset ? total_bytes - shared_data_offset : 0;

    return {
        {RegionKind::kBootstrap, 0, bootstrap_bytes},
        {RegionKind::kAllocator, bootstrap_bytes, allocator_bytes},
        {RegionKind::kCoherence, bootstrap_bytes + allocator_bytes, coherence_bytes},
        {RegionKind::kQueues, bootstrap_bytes + allocator_bytes + coherence_bytes, queues_bytes},
        {RegionKind::kSharedData, shared_data_offset, shared_data_bytes},
    };
}

LoomMemRuntime::LoomMemRuntime(CxloomConfig config) : config_(std::move(config)) {}

Status LoomMemRuntime::Initialize() {
    const auto status = ConfigValidator::Validate(config_);
    if (!status.ok()) {
        return status;
    }

    layout_ = BuildDefaultLayout(config_.shared_region_bytes);
    allocator_ = std::make_unique<SlabExtentAllocator>(layout_.shared_data.bytes);
    coherence_ = std::make_unique<TokenCoherenceManager>(config_.local_host_id);

    auto init_status = allocator_->Initialize();
    if (!init_status.ok()) {
        return init_status;
    }

    queues_.clear();
    queues_.resize(config_.host_count);
    for (std::size_t producer = 0; producer < config_.host_count; ++producer) {
        queues_[producer].reserve(config_.host_count);
        for (std::size_t consumer = 0; consumer < config_.host_count; ++consumer) {
            queues_[producer].push_back(std::make_unique<SpscQueue>(config_.queue_capacity_entries));
        }
    }

    initialized_ = true;
    Trace("loommem", "initialized shared memory runtime skeleton");
    return Status::Ok();
}

Status LoomMemRuntime::Finalize() {
    initialized_ = false;
    queues_.clear();
    coherence_.reset();
    allocator_.reset();
    return Status::Ok();
}

Result<GlobalPointer> LoomMemRuntime::AllocateShared(std::size_t bytes, std::size_t alignment) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before allocation");
    }
    auto result = allocator_->Allocate(bytes, alignment);
    if (!result.ok()) {
        return result.status();
    }
    result.value().offset += layout_.shared_data.offset;
    return result.value();
}

Status LoomMemRuntime::FreeShared(GlobalPointer gptr) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before free");
    }
    if (gptr.region_id != 0 || gptr.offset < layout_.shared_data.offset ||
        gptr.offset >= layout_.shared_data.offset + layout_.shared_data.bytes) {
        return Status::InvalidArgument("global pointer does not refer to the shared-data region");
    }
    gptr.offset -= layout_.shared_data.offset;
    return allocator_->Free(gptr);
}

Result<HostId> LoomMemRuntime::ResolvePreferredHost(const GlobalPointer& gptr) const {
    if (config_.host_count == 0) {
        return Status::FailedPrecondition("host_count is zero");
    }
    return static_cast<HostId>((gptr.offset / config_.per_host_extent_bytes) % config_.host_count);
}

Result<SpscQueue*> LoomMemRuntime::GetQueue(HostId producer, HostId consumer) {
    if (producer >= config_.host_count || consumer >= config_.host_count) {
        return Status::InvalidArgument("queue endpoint is out of range");
    }
    return queues_[producer][consumer].get();
}

}  // namespace cxloom::loommem
