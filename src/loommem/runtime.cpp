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
        allocator_ = std::make_unique<SharedBumpAllocator>(allocator_header_, region_mapper_.base(),
                                                           region_mapper_.bytes(), config_.local_host_id,
                                                           layout_.shared_data.offset, layout_.shared_data.bytes);
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
        auto* shared_allocator = dynamic_cast<SharedBumpAllocator*>(allocator_.get());
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
    const auto unmap_status = region_mapper_.Unmap();
    if (!unmap_status.ok()) {
        return unmap_status;
    }
    return poller_status;
}

Result<GlobalPointer> LoomMemRuntime::AllocateShared(std::size_t bytes, std::size_t alignment) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before allocation");
    }
    auto result = allocator_->Allocate(bytes, alignment);
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
    }
    return allocator_->Free(gptr);
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
    const auto* allocator = dynamic_cast<const SharedBumpAllocator*>(allocator_.get());
    if (allocator == nullptr) {
        return Status::FailedPrecondition("allocation descriptors require a shared runtime");
    }
    return allocator->Describe(gptr);
}

Result<HostId> LoomMemRuntime::ResolveOwningHost(GlobalPointer gptr) const {
    if (!initialized_)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto* allocator = dynamic_cast<const SharedBumpAllocator*>(allocator_.get());
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
        if (message.header.kind == MessageKind::kTokenReq || message.header.kind == MessageKind::kTokenGrant) {
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

Result<TokenRequestHandle> LoomMemRuntime::RequestWriteToken(GlobalPointer object) {
    if (!initialized_ || token_service_ == nullptr)
        return Status::FailedPrecondition("write tokens require an initialized shared runtime");
    if (queue_poller_ == nullptr || !queue_poller_->running())
        return Status::FailedPrecondition("write tokens require a running queue poller");
    return token_service_->Request(object);
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

std::size_t LoomMemRuntime::cached_replica_count() const {
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    return replicas_.size();
}

std::size_t LoomMemRuntime::cached_replica_bytes() const {
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    return cached_replica_bytes_;
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

void LoomMemRuntime::CacheReplica(std::uint64_t object_offset, std::uint64_t generation, Version version,
                                  std::shared_ptr<const std::vector<std::byte>> storage) {
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    const auto existing = replicas_.find(object_offset);
    if (existing != replicas_.end()) {
        cached_replica_bytes_ -= existing->second.storage->size();
        replicas_.erase(existing);
    }
    cached_replica_bytes_ += storage->size();
    replicas_.emplace(object_offset,
                      CachedReplica {generation, version, std::move(storage), ++replica_access_clock_});
    EvictReplicasLocked();
}

Result<ReadSnapshot> LoomMemRuntime::AcquireReadSnapshot(GlobalPointer object, std::uint64_t timeout_ms) {
    if (!initialized_ || !region_mapper_.is_shared() || timeout_ms == 0)
        return Status::FailedPrecondition("read snapshots require an initialized shared runtime and timeout");
    auto* allocator = dynamic_cast<SharedBumpAllocator*>(allocator_.get());
    if (allocator == nullptr)
        return Status::FailedPrecondition("read snapshots require the shared allocator");
    const auto descriptor_result = allocator->MutableDescriptor(object);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto metadata_status = AcquireData(descriptor, sizeof(*descriptor), VisibilityMode::kReleaseAcquire);
        if (!metadata_status.ok())
            return metadata_status;
        const auto epoch_before = descriptor->coherence_epoch.load(std::memory_order_acquire);
        if ((epoch_before & 1U) != 0) {
            std::this_thread::yield();
            continue;
        }
        const auto version_before = descriptor->version.load(std::memory_order_acquire);
        const auto generation = descriptor->generation;
        {
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            const auto cached = replicas_.find(object.offset);
            if (cached != replicas_.end() && cached->second.generation == generation &&
                cached->second.version == version_before) {
                cached->second.last_access = ++replica_access_clock_;
                return ReadSnapshot {object, version_before, cached->second.storage};
            }
        }

        auto refreshed = std::make_shared<std::vector<std::byte>>(descriptor->bytes);
        const auto data_status = AcquireData(static_cast<std::byte*>(region_mapper_.base()) + object.offset,
                                             descriptor->bytes, VisibilityMode::kReleaseAcquire);
        if (!data_status.ok())
            return data_status;
        std::memcpy(refreshed->data(), static_cast<std::byte*>(region_mapper_.base()) + object.offset,
                    descriptor->bytes);
        const auto verify_status = AcquireData(descriptor, sizeof(*descriptor), VisibilityMode::kReleaseAcquire);
        if (!verify_status.ok())
            return verify_status;
        const auto epoch_after = descriptor->coherence_epoch.load(std::memory_order_acquire);
        const auto version_after = descriptor->version.load(std::memory_order_acquire);
        if (epoch_before != epoch_after || (epoch_after & 1U) != 0 || version_before != version_after ||
            generation != descriptor->generation) {
            std::this_thread::yield();
            continue;
        }
        std::shared_ptr<const std::vector<std::byte>> immutable = std::move(refreshed);
        CacheReplica(object.offset, generation, version_after, immutable);
        return ReadSnapshot {object, version_after, std::move(immutable)};
    }
    return Status::Unavailable("timed out waiting for a stable readable object version");
}

Result<WriteBuffer> LoomMemRuntime::AcquireWriteBuffer(GlobalPointer object, std::uint64_t timeout_ms) {
    if (!initialized_ || token_service_ == nullptr)
        return Status::FailedPrecondition("write buffers require an initialized shared runtime");
    // Buffered writers do not touch shared bytes until release, so holding the
    // token alone must not make the last committed version unreadable.
    const auto request = token_service_->Request(object, false);
    if (!request.ok())
        return request.status();
    const auto lease = WaitForWriteToken(request.value(), timeout_ms);
    if (!lease.ok()) {
        CancelWriteTokenRequest(request.value());
        return lease.status();
    }
    const auto allocation = DescribeSharedAllocation(object);
    if (!allocation.ok()) {
        token_service_->Release(lease.value(), false);
        return allocation.status();
    }
    auto storage = std::make_shared<std::vector<std::byte>>(allocation.value().bytes);
    const auto data_status = AcquireData(static_cast<std::byte*>(region_mapper_.base()) + object.offset,
                                         allocation.value().bytes, VisibilityMode::kReleaseAcquire);
    if (!data_status.ok()) {
        token_service_->Release(lease.value(), false);
        return data_status;
    }
    std::memcpy(storage->data(), static_cast<std::byte*>(region_mapper_.base()) + object.offset,
                allocation.value().bytes);
    return WriteBuffer {lease.value(), std::move(storage)};
}

Status LoomMemRuntime::ReleaseWriteBuffer(const WriteBuffer& write) {
    if (!initialized_ || write.storage == nullptr)
        return Status::FailedPrecondition("write buffer requires an initialized runtime and storage");
    const auto allocation = DescribeSharedAllocation(write.lease.object);
    if (!allocation.ok())
        return allocation.status();
    if (allocation.value().generation != write.lease.generation || allocation.value().bytes != write.storage->size())
        return Status::FailedPrecondition("write buffer does not match the shared allocation");
    const auto begin_status = token_service_->BeginWriteback(write.lease);
    if (!begin_status.ok())
        return begin_status;
    std::memcpy(static_cast<std::byte*>(region_mapper_.base()) + write.lease.object.offset, write.storage->data(),
                write.storage->size());
    const auto release_status = ReleaseWriteToken(write.lease);
    if (!release_status.ok())
        return release_status;
    std::shared_ptr<const std::vector<std::byte>> immutable =
        std::make_shared<const std::vector<std::byte>>(*write.storage);
    CacheReplica(write.lease.object.offset, write.lease.generation, write.lease.version + 1, immutable);
    return Status::Ok();
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
                                                        layout_.shared_data.offset, layout_.shared_data.bytes);
    if (!allocator_status.ok()) {
        bootstrap_->state.store(static_cast<std::uint32_t>(BootstrapState::kFailed), std::memory_order_release);
        return allocator_status;
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
