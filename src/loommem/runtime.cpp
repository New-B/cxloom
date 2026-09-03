#include "cxloom/loommem/runtime.h"

#include <chrono>
#include <cstring>
#include <new>
#include <thread>

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

namespace {

bool SameRange(const RegionRange& lhs, const RegionRange& rhs) {
    return lhs.kind == rhs.kind && lhs.offset == rhs.offset && lhs.bytes == rhs.bytes;
}

bool SameLayout(const SharedRegionLayout& lhs, const SharedRegionLayout& rhs) {
    return SameRange(lhs.bootstrap, rhs.bootstrap) && SameRange(lhs.allocator, rhs.allocator) &&
           SameRange(lhs.coherence, rhs.coherence) && SameRange(lhs.queues, rhs.queues) &&
           SameRange(lhs.shared_data, rhs.shared_data);
}

}  // namespace

LoomMemRuntime::LoomMemRuntime(CxloomConfig config) : config_(std::move(config)) {}

Status LoomMemRuntime::Initialize() {
    const auto status = ConfigValidator::Validate(config_);
    if (!status.ok()) {
        return status;
    }

    layout_ = BuildDefaultLayout(config_.shared_region_bytes);
    if (layout_.shared_data.bytes == 0 || layout_.bootstrap.bytes < sizeof(BootstrapHeader)) {
        return Status::InvalidArgument("shared_region_bytes is too small for the LoomMem layout");
    }

    const auto map_status = region_mapper_.Map(config_);
    if (!map_status.ok()) {
        return map_status;
    }
    bootstrap_ = static_cast<BootstrapHeader*>(region_mapper_.base());

    const bool initialize_bootstrap = !region_mapper_.is_shared() || config_.bootstrap_owner;
    const auto bootstrap_status = initialize_bootstrap ? InitializeBootstrap() : AttachBootstrap();
    if (!bootstrap_status.ok()) {
        bootstrap_ = nullptr;
        region_mapper_.Unmap();
        return bootstrap_status;
    }

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
    Trace("loommem", region_mapper_.is_shared() ? "initialized shared CXL region" :
                                                 "initialized private test region");
    return Status::Ok();
}

Status LoomMemRuntime::Finalize() {
    initialized_ = false;
    queues_.clear();
    coherence_.reset();
    allocator_.reset();
    bootstrap_ = nullptr;
    const auto unmap_status = region_mapper_.Unmap();
    if (!unmap_status.ok()) {
        return unmap_status;
    }
    return Status::Ok();
}

Result<GlobalPointer> LoomMemRuntime::AllocateShared(std::size_t bytes, std::size_t alignment) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before allocation");
    }
    if (region_mapper_.is_shared()) {
        return Status::Unimplemented("shared allocator metadata is not implemented yet");
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
    if (region_mapper_.is_shared()) {
        return Status::Unimplemented("shared allocator metadata is not implemented yet");
    }
    if (gptr.region_id != 0 || gptr.offset < layout_.shared_data.offset ||
        gptr.offset >= layout_.shared_data.offset + layout_.shared_data.bytes) {
        return Status::InvalidArgument("global pointer does not refer to the shared-data region");
    }
    gptr.offset -= layout_.shared_data.offset;
    return allocator_->Free(gptr);
}

Result<void*> LoomMemRuntime::ResolveLocal(const GlobalPointer& gptr) const {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before address resolution");
    }
    if (gptr.region_id != 0 || gptr.offset >= region_mapper_.bytes()) {
        return Status::InvalidArgument("global pointer is outside the mapped shared region");
    }
    auto* base = static_cast<std::byte*>(region_mapper_.base());
    return base + gptr.offset;
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

Status LoomMemRuntime::InitializeBootstrap() {
    std::memset(region_mapper_.base(), 0, layout_.bootstrap.bytes);
    bootstrap_ = new (region_mapper_.base()) BootstrapHeader();
    bootstrap_->state.store(static_cast<std::uint32_t>(BootstrapState::kInitializing), std::memory_order_relaxed);
    bootstrap_->magic = kBootstrapMagic;
    bootstrap_->layout_version = kBootstrapLayoutVersion;
    bootstrap_->header_bytes = sizeof(BootstrapHeader);
    bootstrap_->region_bytes = config_.shared_region_bytes;
    bootstrap_->host_count = config_.host_count;
    bootstrap_->layout = layout_;
    bootstrap_->state.store(static_cast<std::uint32_t>(BootstrapState::kReady), std::memory_order_release);
    return Status::Ok();
}

Status LoomMemRuntime::AttachBootstrap() {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config_.bootstrap_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = static_cast<BootstrapState>(bootstrap_->state.load(std::memory_order_acquire));
        if (state == BootstrapState::kReady) {
            const auto validation = ValidateBootstrap(*bootstrap_);
            if (!validation.ok()) {
                return validation;
            }
            layout_ = bootstrap_->layout;
            return Status::Ok();
        }
        if (state == BootstrapState::kFailed) {
            return Status::Unavailable("shared bootstrap initialization failed on its owner host");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Status::Unavailable("timed out waiting for shared bootstrap readiness");
}

Status LoomMemRuntime::ValidateBootstrap(const BootstrapHeader& header) const {
    if (header.magic != kBootstrapMagic || header.layout_version != kBootstrapLayoutVersion ||
        header.header_bytes != sizeof(BootstrapHeader)) {
        return Status::FailedPrecondition("shared region has an incompatible LoomMem bootstrap header");
    }
    if (header.region_bytes != config_.shared_region_bytes || header.host_count != config_.host_count) {
        return Status::FailedPrecondition("shared region bootstrap does not match this runtime configuration");
    }
    if (!SameLayout(header.layout, layout_)) {
        return Status::FailedPrecondition("shared region layout differs from this LoomMem build configuration");
    }
    return Status::Ok();
}

}  // namespace cxloom::loommem
