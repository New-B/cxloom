#include "cxloom/loommem/runtime.h"

#include <algorithm>
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

Result<std::size_t> AutomaticQueueCapacity(std::uint16_t host_count, std::uint64_t region_bytes) {
    if (host_count <= 1)
        return std::size_t {1};
    const auto queue_count = static_cast<std::uint64_t>(host_count) * (host_count - 1);
    if (region_bytes <= sizeof(QueueRegionHeader) ||
        (region_bytes - sizeof(QueueRegionHeader)) / queue_count <= sizeof(SharedSpscQueueHeader))
        return Status::InvalidArgument("queue region cannot hold one queue for every directed host pair");
    const auto capacity = ((region_bytes - sizeof(QueueRegionHeader)) / queue_count -
                           sizeof(SharedSpscQueueHeader)) /
                          sizeof(SharedSpscQueueSlot);
    return static_cast<std::size_t>(std::min<std::uint64_t>(capacity, 1024));
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
    if (config_.queue_capacity_entries == 0) {
        const auto capacity = AutomaticQueueCapacity(config_.host_count, layout_.queues.bytes);
        if (!capacity.ok())
            return capacity.status();
        config_.queue_capacity_entries = capacity.value();
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

    allocator_header_ =
        reinterpret_cast<AllocatorHeader*>(static_cast<std::byte*>(region_mapper_.base()) + layout_.allocator.offset);
    coherence_header_ = reinterpret_cast<CoherenceRegionHeader*>(
        static_cast<std::byte*>(region_mapper_.base()) + layout_.coherence.offset);

    auto* queue_region = static_cast<std::byte*>(region_mapper_.base()) + layout_.queues.offset;
    const auto queue_region_status =
        ValidateSharedQueueRegion(queue_region, layout_.queues.bytes, config_.host_count, config_.queue_capacity_entries);
    if (!queue_region_status.ok()) {
        allocator_header_ = nullptr;
        bootstrap_ = nullptr;
        region_mapper_.Unmap();
        return queue_region_status;
    }

    if (region_mapper_.is_shared()) {
        allocator_ = std::make_unique<SharedExtentAllocator>(allocator_header_, region_mapper_.base(),
                                                           region_mapper_.bytes(), config_.local_host_id,
                                                           layout_.shared_data.offset, layout_.shared_data.bytes,
                                                           coherence_header_, config_.default_coherence_granularity,
                                                           config_.coherence_granule_bytes);
    } else {
        allocator_ = std::make_unique<SlabExtentAllocator>(layout_.shared_data.bytes);
    }
    coherence_ = std::make_unique<TokenCoherenceManager>(config_.local_host_id);

    auto init_status = allocator_->Initialize();
    if (!init_status.ok()) {
        coherence_.reset();
        allocator_.reset();
        allocator_header_ = nullptr;
        bootstrap_ = nullptr;
        region_mapper_.Unmap();
        return init_status;
    }

    const auto registration_status = RegisterLocalHost();
    if (!registration_status.ok()) {
        coherence_.reset();
        allocator_.reset();
        allocator_header_ = nullptr;
        bootstrap_ = nullptr;
        region_mapper_.Unmap();
        return registration_status;
    }

    queues_.clear();
    queues_.resize(config_.host_count);
    for (HostId producer = 0; producer < config_.host_count; ++producer) {
        queues_[producer].reserve(config_.host_count);
        for (HostId consumer = 0; consumer < config_.host_count; ++consumer) {
            if (producer == consumer) {
                queues_[producer].push_back(nullptr);
                continue;
            }
            const auto storage = LocateSharedQueue(queue_region, layout_.queues.bytes, producer, consumer);
            if (!storage.ok()) {
                queues_.clear();
                coherence_.reset();
                allocator_.reset();
                allocator_header_ = nullptr;
                bootstrap_ = nullptr;
                region_mapper_.Unmap();
                return storage.status();
            }
            queues_[producer].push_back(std::make_unique<SpscQueue>(storage.value(), config_.local_host_id));
        }
    }

    if (region_mapper_.is_shared()) {
        auto* shared_allocator = dynamic_cast<SharedExtentAllocator*>(allocator_.get());
        token_service_ = std::make_unique<TokenService>(
            config_.local_host_id, shared_allocator, region_mapper_.base(),
            [this](HostId producer, HostId consumer) { return GetQueue(producer, consumer); });
    }

    initialized_ = true;
    Trace("loommem", region_mapper_.is_shared() ? "initialized shared CXL region" : "initialized private test region");
    return Status::Ok();
}

Status LoomMemRuntime::Finalize() {
    initialized_ = false;
    const auto poller_status = StopQueuePoller();
    queue_poller_.reset();
    token_service_.reset();
    {
        std::lock_guard<std::mutex> lock(replicas_mutex_);
        replicas_.clear();
        cached_replica_bytes_ = 0;
        replica_access_clock_ = 0;
    }
    queues_.clear();
    coherence_.reset();
    allocator_.reset();
    bootstrap_ = nullptr;
    allocator_header_ = nullptr;
    coherence_header_ = nullptr;
    const auto unmap_status = region_mapper_.Unmap();
    if (!unmap_status.ok()) {
        return unmap_status;
    }
    return poller_status;
}

Result<GlobalPointer> LoomMemRuntime::AllocateShared(std::size_t bytes, std::size_t alignment) {
    return AllocateShared(AllocationOptions {bytes, alignment, config_.default_coherence_granularity,
                                             config_.coherence_granule_bytes});
}

Result<GlobalPointer> LoomMemRuntime::AllocateShared(const AllocationOptions& options) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before allocation");
    }
    auto* shared_allocator = dynamic_cast<SharedExtentAllocator*>(allocator_.get());
    auto result = shared_allocator == nullptr ? allocator_->Allocate(options.bytes, options.alignment)
                                              : shared_allocator->Allocate(options);
    if (!result.ok()) {
        return result.status();
    }
    if (!region_mapper_.is_shared()) {
        result.value().offset += layout_.shared_data.offset;
    } else if (token_service_ != nullptr) {
        const auto token_status = token_service_->RegisterAllocation(result.value());
        if (!token_status.ok())
            return token_status;
    }
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
    if (!region_mapper_.is_shared()) {
        gptr.offset -= layout_.shared_data.offset;
        return allocator_->Free(gptr);
    }
    auto* shared_allocator = dynamic_cast<SharedExtentAllocator*>(allocator_.get());
    const auto allocation = shared_allocator->Describe(gptr);
    if (!allocation.ok())
        return allocation.status();
    const auto descriptor_result = shared_allocator->MutableDescriptor(gptr);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    const auto retire_status = token_service_->PrepareRetire(gptr, config_.bootstrap_timeout_ms);
    if (!retire_status.ok())
        return retire_status;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config_.bootstrap_timeout_ms);
    while (true) {
        bool quiescent = true;
        for (HostId host = 0; host < config_.host_count; ++host) {
            if (descriptor->active_references[host].load(std::memory_order_acquire) != 0) {
                quiescent = false;
                break;
            }
        }
        for (std::uint64_t block = 0; quiescent && block < descriptor->coherence_block_count; ++block) {
            auto* metadata = reinterpret_cast<CoherenceBlockDescriptor*>(
                static_cast<std::byte*>(region_mapper_.base()) + descriptor->coherence_metadata_offset) + block;
            if ((metadata->writeback_epoch.load(std::memory_order_acquire) & 1U) != 0)
                quiescent = false;
        }
        if (quiescent)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            shared_allocator->CancelRetire(descriptor, allocation.value().allocation_id);
            return Status::Unavailable("timed out waiting for all hosts to release object references");
        }
        std::this_thread::yield();
    }
    const auto free_status = shared_allocator->Free(gptr);
    if (!free_status.ok()) {
        shared_allocator->CancelRetire(descriptor, allocation.value().allocation_id);
        return free_status;
    }
    token_service_->ForgetAllocation(gptr, allocation.value().coherence_metadata_offset,
                                     allocation.value().coherence_block_count);
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    for (auto it = replicas_.begin(); it != replicas_.end();) {
        if (it->second.object_offset == gptr.offset) {
            cached_replica_bytes_ -= it->second.storage->size();
            it = replicas_.erase(it);
        } else {
            ++it;
        }
    }
    return Status::Ok();
}

Result<ObjectReference> LoomMemRuntime::AcquireObjectReference(GlobalPointer gptr) {
    if (!initialized_ || !region_mapper_.is_shared())
        return Status::FailedPrecondition("object references require an initialized shared runtime");
    auto* allocator = dynamic_cast<SharedExtentAllocator*>(allocator_.get());
    const auto descriptor = allocator->AcquireReference(gptr, config_.local_host_id);
    if (!descriptor.ok())
        return descriptor.status();
    const auto allocation_id = descriptor.value()->allocation_id;
    std::shared_ptr<void> guard(descriptor.value(), [allocator, allocation_id, host = config_.local_host_id](void* p) {
        allocator->ReleaseReference(static_cast<AllocationDescriptor*>(p), allocation_id, host);
    });
    return ObjectReference {gptr, allocation_id, std::move(guard)};
}

Result<void*> LoomMemRuntime::ResolveLocal(const GlobalPointer& gptr) const {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before address resolution");
    }
    if (gptr.region_id != 0 || gptr.offset < layout_.shared_data.offset ||
        gptr.offset >= layout_.shared_data.offset + layout_.shared_data.bytes) {
        return Status::InvalidArgument("global pointer is outside the mapped shared region");
    }
    if (region_mapper_.is_shared()) {
        const auto allocation = DescribeSharedAllocation(gptr);
        if (!allocation.ok())
            return allocation.status();
    }
    auto* base = static_cast<std::byte*>(region_mapper_.base());
    return base + gptr.offset;
}

Result<AllocationInfo> LoomMemRuntime::DescribeSharedAllocation(GlobalPointer gptr) const {
    if (!initialized_)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto* allocator = dynamic_cast<const SharedExtentAllocator*>(allocator_.get());
    if (allocator == nullptr) {
        return Status::FailedPrecondition("allocation descriptors require a shared runtime");
    }
    return allocator->Describe(gptr);
}

Result<HostId> LoomMemRuntime::ResolveOwningHost(GlobalPointer gptr) const {
    if (!initialized_)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto* allocator = dynamic_cast<const SharedExtentAllocator*>(allocator_.get());
    if (allocator == nullptr) {
        return Status::FailedPrecondition("allocation ownership requires a shared runtime");
    }
    return allocator->OwningHost(gptr);
}

Status LoomMemRuntime::PublishBootstrapProbe(std::uint64_t value) {
    if (!initialized_ || bootstrap_ == nullptr)
        return Status::FailedPrecondition("runtime is not initialized");
    auto& host = bootstrap_->hosts[config_.local_host_id];
    host.probe_value.store(value, std::memory_order_relaxed);
    host.state.store(static_cast<std::uint32_t>(HostRegistrationState::kProbeReady), std::memory_order_release);
    return Status::Ok();
}

Status LoomMemRuntime::WaitForAllHosts(std::uint64_t timeout_ms) const {
    if (!initialized_ || bootstrap_ == nullptr)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (HostId host = 0; host < config_.host_count; ++host) {
            const auto state =
                static_cast<HostRegistrationState>(bootstrap_->hosts[host].state.load(std::memory_order_acquire));
            if (state < HostRegistrationState::kProbeReady) {
                ready = false;
                break;
            }
        }
        if (ready)
            return Status::Ok();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Status::Unavailable("timed out waiting for all hosts to publish bootstrap probes");
}

Result<std::uint64_t> LoomMemRuntime::ReadBootstrapProbe(HostId host) const {
    if (!initialized_ || bootstrap_ == nullptr)
        return Status::FailedPrecondition("runtime is not initialized");
    if (host >= config_.host_count)
        return Status::InvalidArgument("probe host is out of range");
    const auto state =
        static_cast<HostRegistrationState>(bootstrap_->hosts[host].state.load(std::memory_order_acquire));
    if (state < HostRegistrationState::kProbeReady)
        return Status::Unavailable("host probe is not ready");
    return bootstrap_->hosts[host].probe_value.load(std::memory_order_relaxed);
}

std::uint32_t LoomMemRuntime::joined_host_count() const {
    return bootstrap_ == nullptr ? 0 : bootstrap_->joined_hosts.load(std::memory_order_acquire);
}

Status LoomMemRuntime::PublishSharedObject(GlobalPointer gptr, std::uint64_t bytes, VisibilityMode mode) {
    if (!initialized_ || bootstrap_ == nullptr || allocator_header_ == nullptr) {
        return Status::FailedPrecondition("runtime is not initialized");
    }
    if (bytes == 0 || gptr.region_id != 0 || gptr.offset > region_mapper_.bytes() ||
        bytes > region_mapper_.bytes() - gptr.offset) {
        return Status::InvalidArgument("published object is outside the mapped region");
    }
    const auto allocation = DescribeSharedAllocation(gptr);
    if (!allocation.ok())
        return allocation.status();
    if (allocation.value().owner_host != config_.local_host_id || bytes > allocation.value().bytes) {
        return Status::InvalidArgument("published object is not owned by the local host allocation");
    }
    if (allocation.value().coherence_metadata_offset != 0) {
        auto* blocks = static_cast<std::byte*>(region_mapper_.base()) +
                       allocation.value().coherence_metadata_offset;
        const auto block_status = PublishData(
            blocks, allocation.value().coherence_block_count * sizeof(CoherenceBlockDescriptor), mode);
        if (!block_status.ok())
            return block_status;
    }
    auto* descriptor = static_cast<std::byte*>(region_mapper_.base()) + gptr.offset - sizeof(AllocationDescriptor);
    const auto descriptor_status = PublishData(descriptor, sizeof(AllocationDescriptor), mode);
    if (!descriptor_status.ok())
        return descriptor_status;

    auto& host = bootstrap_->hosts[config_.local_host_id];
    host.object_offset.store(gptr.offset, std::memory_order_relaxed);
    host.object_bytes.store(bytes, std::memory_order_relaxed);
    host.state.store(static_cast<std::uint32_t>(HostRegistrationState::kObjectReady), std::memory_order_release);
    return PublishData(&host, sizeof(host), mode);
}

Status LoomMemRuntime::WaitForAllSharedObjects(std::uint64_t timeout_ms, VisibilityMode mode) const {
    if (!initialized_ || bootstrap_ == nullptr) {
        return Status::FailedPrecondition("runtime is not initialized");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (HostId host = 0; host < config_.host_count; ++host) {
            auto& registration = bootstrap_->hosts[host];
            const auto acquire_status = AcquireData(&registration, sizeof(registration), mode);
            if (!acquire_status.ok())
                return acquire_status;
            const auto state = static_cast<HostRegistrationState>(registration.state.load(std::memory_order_acquire));
            if (state != HostRegistrationState::kObjectReady) {
                ready = false;
                break;
            }
        }
        if (ready)
            return Status::Ok();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Status::Unavailable("timed out waiting for all hosts to publish shared objects");
}

Result<PublishedSharedObject> LoomMemRuntime::ReadPublishedSharedObject(HostId host) const {
    if (!initialized_ || bootstrap_ == nullptr) {
        return Status::FailedPrecondition("runtime is not initialized");
    }
    if (host >= config_.host_count)
        return Status::InvalidArgument("published-object host is out of range");
    const auto& registration = bootstrap_->hosts[host];
    const auto state = static_cast<HostRegistrationState>(registration.state.load(std::memory_order_acquire));
    if (state != HostRegistrationState::kObjectReady) {
        return Status::Unavailable("host shared object is not ready");
    }
    return PublishedSharedObject {GlobalPointer {0, registration.object_offset.load(std::memory_order_relaxed)},
                                  registration.object_bytes.load(std::memory_order_relaxed)};
}

Status LoomMemRuntime::PublishVisibilitySequence(std::uint64_t sequence, VisibilityMode mode) {
    if (!initialized_ || bootstrap_ == nullptr || sequence == 0) {
        return Status::FailedPrecondition("runtime and non-zero sequence are required");
    }
    auto& host = bootstrap_->hosts[config_.local_host_id];
    const auto state = static_cast<HostRegistrationState>(host.state.load(std::memory_order_acquire));
    if (state != HostRegistrationState::kObjectReady) {
        return Status::FailedPrecondition("shared object must be published before visibility iterations");
    }
    host.published_sequence.store(sequence, std::memory_order_release);
    return PublishData(&host.published_sequence, sizeof(host.published_sequence), mode);
}

Status LoomMemRuntime::WaitForVisibilitySequence(std::uint64_t sequence, std::uint64_t timeout_ms,
                                                       VisibilityMode mode) const {
    if (!initialized_ || bootstrap_ == nullptr)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (HostId host = 0; host < config_.host_count; ++host) {
            auto& published = bootstrap_->hosts[host].published_sequence;
            const auto acquire_status = AcquireData(&published, sizeof(published), mode);
            if (!acquire_status.ok())
                return acquire_status;
            if (published.load(std::memory_order_acquire) < sequence) {
                ready = false;
                break;
            }
        }
        if (ready)
            return Status::Ok();
        std::this_thread::yield();
    }
    return Status::Unavailable("timed out waiting for visibility publication sequence");
}

Status LoomMemRuntime::PublishObservedSequence(std::uint64_t sequence, std::uint64_t errors, VisibilityMode mode) {
    if (!initialized_ || bootstrap_ == nullptr || sequence == 0) {
        return Status::FailedPrecondition("runtime and non-zero sequence are required");
    }
    auto& host = bootstrap_->hosts[config_.local_host_id];
    host.visibility_errors.store(errors, std::memory_order_relaxed);
    host.observed_sequence.store(sequence, std::memory_order_release);
    return PublishData(&host.observed_sequence, sizeof(host.observed_sequence), mode);
}

Status LoomMemRuntime::WaitForObservedSequence(std::uint64_t sequence, std::uint64_t timeout_ms,
                                                     VisibilityMode mode) const {
    if (!initialized_ || bootstrap_ == nullptr)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (HostId host = 0; host < config_.host_count; ++host) {
            auto& observed = bootstrap_->hosts[host].observed_sequence;
            const auto acquire_status = AcquireData(&observed, sizeof(observed), mode);
            if (!acquire_status.ok())
                return acquire_status;
            if (observed.load(std::memory_order_acquire) < sequence) {
                ready = false;
                break;
            }
        }
        if (ready)
            return Status::Ok();
        std::this_thread::yield();
    }
    return Status::Unavailable("timed out waiting for visibility observation sequence");
}

std::uint64_t LoomMemRuntime::visibility_error_count() const {
    if (!initialized_ || bootstrap_ == nullptr)
        return 0;
    std::uint64_t errors = 0;
    for (HostId host = 0; host < config_.host_count; ++host) {
        bootstrap_->hosts[host].observed_sequence.load(std::memory_order_acquire);
        errors += bootstrap_->hosts[host].visibility_errors.load(std::memory_order_relaxed);
    }
    return errors;
}

Result<HostId> LoomMemRuntime::ResolvePreferredHost(const GlobalPointer& gptr) const {
    if (config_.host_count == 0)
        return Status::FailedPrecondition("host_count is zero");
    if (region_mapper_.is_shared())
        return ResolveOwningHost(gptr);
    if (gptr.offset < layout_.shared_data.offset) {
        return Status::InvalidArgument("global pointer is outside the shared-data region");
    }
    return static_cast<HostId>(((gptr.offset - layout_.shared_data.offset) / config_.per_host_extent_bytes) %
                               config_.host_count);
}

Result<SpscQueue*> LoomMemRuntime::GetQueue(HostId producer, HostId consumer) {
    if (producer >= config_.host_count || consumer >= config_.host_count) {
        return Status::InvalidArgument("queue endpoint is out of range");
    }
    if (producer == consumer) {
        return Status::InvalidArgument("self-pairs do not use the shared queue transport");
    }
    return queues_[producer][consumer].get();
}

Status LoomMemRuntime::StartQueuePoller(QueueMessageHandler handler, QueuePollerOptions options) {
    if (!initialized_)
        return Status::FailedPrecondition("runtime must be initialized before starting the queue poller");
    if (queue_poller_ != nullptr)
        return Status::AlreadyExists("runtime already owns a queue poller");

    std::vector<SpscQueue*> inbound_queues;
    inbound_queues.reserve(config_.host_count > 0 ? config_.host_count - 1 : 0);
    for (HostId producer = 0; producer < config_.host_count; ++producer) {
        if (producer != config_.local_host_id)
            inbound_queues.push_back(queues_[producer][config_.local_host_id].get());
    }
    auto dispatch = [this, application_handler = std::move(handler)](QueueEnvelope message) mutable {
        if (message.header.kind == MessageKind::kTokenReq || message.header.kind == MessageKind::kTokenGrant ||
            message.header.kind == MessageKind::kTokenReject || message.header.kind == MessageKind::kTokenCancel ||
            message.header.kind == MessageKind::kTokenCancelAck ||
            message.header.kind == MessageKind::kTokenRetire ||
            message.header.kind == MessageKind::kTokenRetireAck) {
            if (token_service_ == nullptr)
                return Status::FailedPrecondition("token message received without a token service");
            return token_service_->HandleMessage(message);
        }
        if (!application_handler)
            return Status::InvalidArgument("no handler is registered for this queue message kind");
        return application_handler(std::move(message));
    };
    queue_poller_ = std::make_unique<QueuePoller>(config_.local_host_id, std::move(inbound_queues),
                                                  std::move(dispatch), options);
    const auto status = queue_poller_->Start();
    if (!status.ok())
        queue_poller_.reset();
    return status;
}

Status LoomMemRuntime::StopQueuePoller() {
    if (queue_poller_ == nullptr)
        return Status::Ok();
    return queue_poller_->Stop();
}

Result<TokenRequestHandle> LoomMemRuntime::RequestWriteToken(GlobalPointer object, std::uint64_t block_index) {
    if (!initialized_ || token_service_ == nullptr)
        return Status::FailedPrecondition("write tokens require an initialized shared runtime");
    if (queue_poller_ == nullptr || !queue_poller_->running())
        return Status::FailedPrecondition("write tokens require a running queue poller");
    return token_service_->Request(object, block_index);
}

Result<TokenLease> LoomMemRuntime::WaitForWriteToken(const TokenRequestHandle& request,
                                                     std::uint64_t timeout_ms) {
    if (!initialized_ || token_service_ == nullptr)
        return Status::FailedPrecondition("write tokens require an initialized shared runtime");
    return token_service_->Wait(request, timeout_ms);
}

Status LoomMemRuntime::ReleaseWriteToken(const TokenLease& lease) {
    if (!initialized_ || token_service_ == nullptr)
        return Status::FailedPrecondition("write tokens require an initialized shared runtime");
    return token_service_->Release(lease);
}

Status LoomMemRuntime::CancelWriteTokenRequest(const TokenRequestHandle& request) {
    if (!initialized_ || token_service_ == nullptr)
        return Status::FailedPrecondition("write tokens require an initialized shared runtime");
    return token_service_->Cancel(request);
}

Status LoomMemRuntime::CancelWriteTokenRequestAndWait(const TokenRequestHandle& request,
                                                       std::uint64_t timeout_ms) {
    if (!initialized_ || token_service_ == nullptr)
        return Status::FailedPrecondition("write tokens require an initialized shared runtime");
    return token_service_->CancelAndWait(request, timeout_ms);
}

std::size_t LoomMemRuntime::cached_replica_count() const {
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    return replicas_.size();
}

std::size_t LoomMemRuntime::cached_replica_bytes() const {
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    return cached_replica_bytes_;
}

std::size_t LoomMemRuntime::pending_token_request_count() const {
    return token_service_ == nullptr ? 0 : token_service_->pending_request_count();
}

void LoomMemRuntime::EvictReplicasLocked() {
    while (replicas_.size() > config_.replica_cache_capacity_entries ||
           cached_replica_bytes_ > config_.replica_cache_capacity_bytes) {
        auto victim = replicas_.end();
        for (auto candidate = replicas_.begin(); candidate != replicas_.end(); ++candidate) {
            if (victim == replicas_.end() || candidate->second.last_access < victim->second.last_access)
                victim = candidate;
        }
        if (victim == replicas_.end())
            break;
        cached_replica_bytes_ -= victim->second.storage->size();
        replicas_.erase(victim);
    }
}

void LoomMemRuntime::CacheReplica(std::uint64_t cache_key, std::uint64_t object_offset,
                                  std::uint64_t block_index, std::uint64_t allocation_id, Version version,
                                  std::shared_ptr<const std::vector<std::byte>> storage) {
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    const auto existing = replicas_.find(cache_key);
    if (existing != replicas_.end()) {
        cached_replica_bytes_ -= existing->second.storage->size();
        replicas_.erase(existing);
    }
    cached_replica_bytes_ += storage->size();
    replicas_.emplace(cache_key, CachedReplica {object_offset, block_index, allocation_id, version,
                                                std::move(storage), ++replica_access_clock_});
    EvictReplicasLocked();
}

Result<ReadSnapshot> LoomMemRuntime::AcquireReadSnapshot(GlobalPointer object, std::uint64_t timeout_ms) {
    const auto allocation = DescribeSharedAllocation(object);
    if (!allocation.ok())
        return allocation.status();
    return AcquireReadRange(object, 0, allocation.value().bytes, timeout_ms, ReadConsistency::kWholeRange);
}

Result<ReadSnapshot> LoomMemRuntime::AcquireReadRange(GlobalPointer object, std::uint64_t offset,
                                                       std::uint64_t bytes, std::uint64_t timeout_ms,
                                                       ReadConsistency consistency) {
    if (!initialized_ || !region_mapper_.is_shared() || timeout_ms == 0)
        return Status::FailedPrecondition("read ranges require an initialized shared runtime and timeout");
    auto* allocator = dynamic_cast<SharedExtentAllocator*>(allocator_.get());
    if (allocator == nullptr)
        return Status::FailedPrecondition("read snapshots require the shared allocator");
    const auto descriptor_result = allocator->AcquireReference(object, config_.local_host_id);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    const auto reference_id = descriptor->allocation_id;
    std::shared_ptr<void> reference_guard(
        descriptor, [allocator, reference_id, host = config_.local_host_id](void* p) {
            allocator->ReleaseReference(static_cast<AllocationDescriptor*>(p), reference_id, host);
        });
    if (bytes == 0 || offset > descriptor->bytes || bytes > descriptor->bytes - offset)
        return Status::InvalidArgument("read range is empty or outside the allocation");
    const auto first_block = offset / descriptor->coherence_block_bytes;
    const auto last_block = (offset + bytes - 1) / descriptor->coherence_block_bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto allocation_id = descriptor->allocation_id;
    while (std::chrono::steady_clock::now() < deadline) {
    const auto range_epoch_before = descriptor->range_commit_epoch.load(std::memory_order_acquire);
    if (consistency == ReadConsistency::kWholeRange && (range_epoch_before & 1U) != 0) {
        std::this_thread::yield();
        continue;
    }
    auto assembled = std::make_shared<std::vector<std::byte>>(bytes);
    std::shared_ptr<const std::vector<std::byte>> single_block_replica;
    std::vector<Version> versions;
    versions.reserve(last_block - first_block + 1);
    for (std::uint64_t index = first_block; index <= last_block; ++index) {
        const auto block_result = allocator->MutableCoherenceBlock(object, index);
        if (!block_result.ok())
            return block_result.status();
        auto* block = block_result.value();
        const auto block_start = index * descriptor->coherence_block_bytes;
        const auto block_bytes = std::min(descriptor->coherence_block_bytes, descriptor->bytes - block_start);
        const auto cache_key = descriptor->coherence_metadata_offset + index * sizeof(CoherenceBlockDescriptor);
        std::shared_ptr<const std::vector<std::byte>> replica;
        Version accepted_version = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto metadata_status = AcquireData(block, sizeof(*block), VisibilityMode::kReleaseAcquire);
            if (!metadata_status.ok())
                return metadata_status;
            const auto epoch_before = block->writeback_epoch.load(std::memory_order_acquire);
            if ((epoch_before & 1U) != 0) {
                std::this_thread::yield();
                continue;
            }
            const auto version_before = block->version.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lock(replicas_mutex_);
                const auto cached = replicas_.find(cache_key);
                if (cached != replicas_.end() && cached->second.allocation_id == allocation_id &&
                    cached->second.version == version_before) {
                    cached->second.last_access = ++replica_access_clock_;
                    replica = cached->second.storage;
                    accepted_version = version_before;
                }
            }
            if (replica != nullptr)
                break;
            auto refreshed = std::make_shared<std::vector<std::byte>>(block_bytes);
            const auto data_status = AcquireData(static_cast<std::byte*>(region_mapper_.base()) + object.offset +
                                                     block_start,
                                                 block_bytes, VisibilityMode::kReleaseAcquire);
            if (!data_status.ok())
                return data_status;
            std::memcpy(refreshed->data(), static_cast<std::byte*>(region_mapper_.base()) + object.offset + block_start,
                        block_bytes);
            const auto verify_status = AcquireData(block, sizeof(*block), VisibilityMode::kReleaseAcquire);
            if (!verify_status.ok())
                return verify_status;
            const auto epoch_after = block->writeback_epoch.load(std::memory_order_acquire);
            const auto version_after = block->version.load(std::memory_order_acquire);
            if (epoch_before != epoch_after || (epoch_after & 1U) != 0 || version_before != version_after ||
                allocation_id != descriptor->allocation_id ||
                descriptor->state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(AllocationState::kAllocated)) {
                std::this_thread::yield();
                continue;
            }
            replica = std::move(refreshed);
            accepted_version = version_after;
            CacheReplica(cache_key, object.offset, index, allocation_id, version_after, replica);
            break;
        }
        if (replica == nullptr)
            return Status::Unavailable("timed out waiting for a stable readable block version");
        if (first_block == last_block && offset == block_start && bytes == block_bytes)
            single_block_replica = replica;
        versions.push_back(accepted_version);
        const auto copy_begin = std::max(offset, block_start);
        const auto copy_end = std::min(offset + bytes, block_start + block_bytes);
        std::memcpy(assembled->data() + (copy_begin - offset), replica->data() + (copy_begin - block_start),
                    copy_end - copy_begin);
    }
    std::shared_ptr<const std::vector<std::byte>> result_storage =
        single_block_replica == nullptr ? std::move(assembled) : std::move(single_block_replica);
    if (consistency == ReadConsistency::kWholeRange) {
        bool stable = true;
        for (std::uint64_t index = first_block; index <= last_block; ++index) {
            const auto block_result = allocator->MutableCoherenceBlock(object, index);
            if (!block_result.ok())
                return block_result.status();
            const auto acquire_status = AcquireData(block_result.value(), sizeof(CoherenceBlockDescriptor),
                                                    VisibilityMode::kReleaseAcquire);
            if (!acquire_status.ok())
                return acquire_status;
            const auto vector_index = index - first_block;
            if ((block_result.value()->writeback_epoch.load(std::memory_order_acquire) & 1U) != 0 ||
                block_result.value()->version.load(std::memory_order_acquire) != versions[vector_index]) {
                stable = false;
                break;
            }
        }
        const auto range_epoch_after = descriptor->range_commit_epoch.load(std::memory_order_acquire);
        if (!stable || range_epoch_before != range_epoch_after || (range_epoch_after & 1U) != 0 ||
            descriptor->allocation_id != allocation_id) {
            std::this_thread::yield();
            continue;
        }
    }
    return ReadSnapshot {object, offset, allocation_id, descriptor->object_version.load(std::memory_order_acquire),
                         std::move(versions), std::move(result_storage)};
    }
    return Status::Unavailable("timed out waiting for a stable whole-range snapshot");
}

Result<WriteBuffer> LoomMemRuntime::AcquireWriteBuffer(GlobalPointer object, std::uint64_t timeout_ms) {
    const auto allocation = DescribeSharedAllocation(object);
    if (!allocation.ok())
        return allocation.status();
    return AcquireWriteRange(object, 0, allocation.value().bytes, timeout_ms, WriteAtomicity::kWholeRange);
}

Result<WriteBuffer> LoomMemRuntime::AcquireWriteRange(GlobalPointer object, std::uint64_t offset,
                                                       std::uint64_t bytes, std::uint64_t timeout_ms,
                                                       WriteAtomicity atomicity) {
    if (!initialized_ || token_service_ == nullptr || timeout_ms == 0)
        return Status::FailedPrecondition("write buffers require an initialized shared runtime");
    // Buffered writers do not touch shared bytes until release, so holding the
    // token alone must not make the last committed version unreadable.
    auto* shared_allocator = dynamic_cast<SharedExtentAllocator*>(allocator_.get());
    if (shared_allocator == nullptr)
        return Status::FailedPrecondition("write ranges require the shared allocator");
    const auto descriptor_result = shared_allocator->AcquireReference(object, config_.local_host_id);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    const auto allocation_id = descriptor->allocation_id;
    std::shared_ptr<void> reference_guard(
        descriptor, [shared_allocator, allocation_id, host = config_.local_host_id](void* p) {
            shared_allocator->ReleaseReference(static_cast<AllocationDescriptor*>(p), allocation_id, host);
        });
    const auto allocation = shared_allocator->Describe(object);
    if (!allocation.ok())
        return allocation.status();
    if (bytes == 0 || offset > allocation.value().bytes || bytes > allocation.value().bytes - offset)
        return Status::InvalidArgument("write range is empty or outside the allocation");
    const auto first_block = offset / allocation.value().coherence_block_bytes;
    const auto last_block = (offset + bytes - 1) / allocation.value().coherence_block_bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<TokenLease> leases;
    for (std::uint64_t index = first_block; index <= last_block; ++index) {
        const auto request = token_service_->Request(object, index, false);
        if (!request.ok()) {
            for (auto it = leases.rbegin(); it != leases.rend(); ++it)
                token_service_->Release(*it, false);
            return request.status();
        }
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now < deadline
                                   ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()
                                   : 0;
        const auto lease = WaitForWriteToken(request.value(), std::max<std::int64_t>(1, remaining));
        if (!lease.ok()) {
            CancelWriteTokenRequestAndWait(request.value(), 1000);
            for (auto it = leases.rbegin(); it != leases.rend(); ++it)
                token_service_->Release(*it, false);
            return lease.status();
        }
        leases.push_back(lease.value());
    }
    auto storage = std::make_shared<std::vector<std::byte>>(bytes);
    const auto data_status = AcquireData(static_cast<std::byte*>(region_mapper_.base()) + object.offset + offset,
                                         bytes, VisibilityMode::kReleaseAcquire);
    if (!data_status.ok()) {
        for (auto it = leases.rbegin(); it != leases.rend(); ++it)
            token_service_->Release(*it, false);
        return data_status;
    }
    std::memcpy(storage->data(), static_cast<std::byte*>(region_mapper_.base()) + object.offset + offset, bytes);
    return WriteBuffer {leases.front(), leases, offset, std::move(storage), std::move(reference_guard), atomicity};
}

Status LoomMemRuntime::ReleaseWriteBuffer(const WriteBuffer& write) {
    if (!initialized_ || write.storage == nullptr)
        return Status::FailedPrecondition("write buffer requires an initialized runtime and storage");
    auto* shared_allocator = dynamic_cast<SharedExtentAllocator*>(allocator_.get());
    const auto descriptor_result = shared_allocator->MutableDescriptor(write.lease.object, true);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    const auto& leases = write.leases.empty() ? std::vector<TokenLease> {write.lease} : write.leases;
    if (descriptor->allocation_id != write.lease.allocation_id || write.offset > descriptor->bytes ||
        write.storage->size() > descriptor->bytes - write.offset)
        return Status::FailedPrecondition("write buffer does not match the shared allocation");
    std::uint64_t range_epoch = 0;
    if (write.atomicity == WriteAtomicity::kWholeRange) {
        while (true) {
            range_epoch = descriptor->range_commit_epoch.load(std::memory_order_acquire);
            if ((range_epoch & 1U) != 0) {
                std::this_thread::yield();
                continue;
            }
            if (descriptor->range_commit_epoch.compare_exchange_weak(
                    range_epoch, range_epoch + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                break;
        }
        const auto epoch_status = PublishData(descriptor, sizeof(*descriptor), VisibilityMode::kReleaseAcquire);
        if (!epoch_status.ok()) {
            descriptor->range_commit_epoch.store(range_epoch + 2, std::memory_order_release);
            for (const auto& lease : leases)
                token_service_->Release(lease, false);
            return epoch_status;
        }
    }
    for (std::size_t lease_index = 0; lease_index < leases.size(); ++lease_index) {
        const auto& lease = leases[lease_index];
        const auto begin_status = token_service_->BeginWriteback(lease);
        if (!begin_status.ok()) {
            if (write.atomicity == WriteAtomicity::kWholeRange)
                descriptor->range_commit_epoch.store(range_epoch + 2, std::memory_order_release);
            for (std::size_t remaining = lease_index; remaining < leases.size(); ++remaining)
                token_service_->Release(leases[remaining], false);
            return begin_status;
        }
        const auto block_start = lease.block_index * descriptor->coherence_block_bytes;
        const auto block_end = std::min(descriptor->bytes, block_start + descriptor->coherence_block_bytes);
        const auto copy_begin = std::max(write.offset, block_start);
        const auto copy_end = std::min(write.offset + write.storage->size(), block_end);
        std::memcpy(static_cast<std::byte*>(region_mapper_.base()) + write.lease.object.offset + copy_begin,
                    write.storage->data() + (copy_begin - write.offset), copy_end - copy_begin);
        const auto release_status = ReleaseWriteToken(lease);
        if (!release_status.ok()) {
            if (write.atomicity == WriteAtomicity::kWholeRange)
                descriptor->range_commit_epoch.store(range_epoch + 2, std::memory_order_release);
            for (std::size_t remaining = lease_index + 1; remaining < leases.size(); ++remaining)
                token_service_->Release(leases[remaining], false);
            return release_status;
        }
    }
    descriptor->object_version.fetch_add(1, std::memory_order_acq_rel);
    if (write.atomicity == WriteAtomicity::kWholeRange)
        descriptor->range_commit_epoch.store(range_epoch + 2, std::memory_order_release);
    const auto publish_status = PublishData(descriptor, sizeof(*descriptor),
                                            VisibilityMode::kReleaseAcquire);
    if (!publish_status.ok())
        return publish_status;
    for (const auto& lease : leases) {
        const auto block_start = lease.block_index * descriptor->coherence_block_bytes;
        const auto block_bytes = std::min(descriptor->coherence_block_bytes, descriptor->bytes - block_start);
        auto cached = std::make_shared<std::vector<std::byte>>(block_bytes);
        std::memcpy(cached->data(), static_cast<std::byte*>(region_mapper_.base()) + write.lease.object.offset + block_start,
                    block_bytes);
        const auto key = descriptor->coherence_metadata_offset +
                         lease.block_index * sizeof(CoherenceBlockDescriptor);
        CacheReplica(key, write.lease.object.offset, lease.block_index, write.lease.allocation_id,
                     lease.version + 1, cached);
    }
    write.reference_guard.reset();
    return Status::Ok();
}

Status LoomMemRuntime::AbortWriteBuffer(const WriteBuffer& write) {
    if (!initialized_ || token_service_ == nullptr || write.storage == nullptr)
        return Status::FailedPrecondition("write buffer requires an initialized runtime and storage");
    const auto& leases = write.leases.empty() ? std::vector<TokenLease> {write.lease} : write.leases;
    Status result = Status::Ok();
    for (auto it = leases.rbegin(); it != leases.rend(); ++it) {
        const auto status = token_service_->Release(*it, false);
        if (!status.ok() && result.ok())
            result = status;
    }
    if (result.ok())
        write.reference_guard.reset();
    return result;
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
    auto* allocator_header =
        reinterpret_cast<AllocatorHeader*>(static_cast<std::byte*>(region_mapper_.base()) + layout_.allocator.offset);
    const auto allocator_status = FormatSharedAllocator(allocator_header, layout_.allocator.bytes,
                                                        layout_.shared_data.offset, layout_.shared_data.bytes,
                                                        layout_.coherence.offset, layout_.coherence.bytes);
    if (!allocator_status.ok()) {
        bootstrap_->state.store(static_cast<std::uint32_t>(BootstrapState::kFailed), std::memory_order_release);
        return allocator_status;
    }
    auto* coherence_header = reinterpret_cast<CoherenceRegionHeader*>(
        static_cast<std::byte*>(region_mapper_.base()) + layout_.coherence.offset);
    const auto coherence_status =
        FormatCoherenceRegion(coherence_header, layout_.coherence.offset, layout_.coherence.bytes);
    if (!coherence_status.ok()) {
        bootstrap_->state.store(static_cast<std::uint32_t>(BootstrapState::kFailed), std::memory_order_release);
        return coherence_status;
    }
    auto* queue_region = static_cast<std::byte*>(region_mapper_.base()) + layout_.queues.offset;
    const auto queue_status = FormatSharedQueueRegion(queue_region, layout_.queues.bytes, config_.host_count,
                                                      config_.queue_capacity_entries);
    if (!queue_status.ok()) {
        bootstrap_->state.store(static_cast<std::uint32_t>(BootstrapState::kFailed), std::memory_order_release);
        return queue_status;
    }
    bootstrap_->state.store(static_cast<std::uint32_t>(BootstrapState::kReady), std::memory_order_release);
    return Status::Ok();
}

Status LoomMemRuntime::AttachBootstrap() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.bootstrap_timeout_ms);
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

Status LoomMemRuntime::RegisterLocalHost() {
    auto& registration = bootstrap_->hosts[config_.local_host_id];
    std::uint32_t expected = static_cast<std::uint32_t>(HostRegistrationState::kEmpty);
    if (!registration.state.compare_exchange_strong(expected,
                                                    static_cast<std::uint32_t>(HostRegistrationState::kJoined),
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
        return Status::AlreadyExists("local host id is already registered in this bootstrap session");
    }
    bootstrap_->joined_hosts.fetch_add(1, std::memory_order_acq_rel);
    return Status::Ok();
}

}  // namespace cxloom::loommem
