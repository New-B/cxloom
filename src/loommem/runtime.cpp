#include "cxloom/common/execution_context.h"
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

    const auto bootstrap_status = config_.bootstrap_owner ? InitializeBootstrap() : AttachBootstrap();
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

    allocator_ = std::make_unique<SharedExtentAllocator>(allocator_header_, region_mapper_.base(),
                                                        region_mapper_.bytes(), config_.local_host_id,
                                                        layout_.shared_data.offset, layout_.shared_data.bytes,
                                                        coherence_header_, config_.coherence_granule_bytes);

    auto init_status = allocator_->Initialize();
    if (!init_status.ok()) {
        allocator_.reset();
        allocator_header_ = nullptr;
        bootstrap_ = nullptr;
        region_mapper_.Unmap();
        return init_status;
    }

    const auto registration_status = RegisterLocalHost();
    if (!registration_status.ok()) {
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
                allocator_.reset();
                allocator_header_ = nullptr;
                bootstrap_ = nullptr;
                region_mapper_.Unmap();
                return storage.status();
            }
            queues_[producer].push_back(std::make_unique<SpscQueue>(storage.value(), config_.local_host_id));
        }
    }

    token_service_ = std::make_unique<TokenService>(
        config_.local_host_id, allocator_.get(), region_mapper_.base(),
        [this](HostId producer, HostId consumer) { return GetQueue(producer, consumer); });

    initialized_ = true;
    Trace("loommem", "initialized shared CXL region");
    return Status::Ok();
}

Status LoomMemRuntime::Finalize() {
    if (initialized_ && allocator_->Retirement().phase != RetirementPhase::kIdle)
        return Status::FailedPrecondition("finish the shared retirement transaction before finalizing LoomMem");
    if (staged_write_count())
        return Status::FailedPrecondition("publish staged writes before finalizing LoomMem");
    initialized_ = false;
    const auto poller_status = StopQueuePoller();
    queue_poller_.reset();
    token_service_.reset();
    {
        std::lock_guard<std::mutex> lock(replicas_mutex_);
        while (!replicas_.empty()) EraseReplicaLocked(replicas_, replicas_.begin());
        while (!old_replicas_.empty()) EraseReplicaLocked(old_replicas_, old_replicas_.begin());
        cached_replica_bytes_ = 0;
        replica_access_clock_ = 0;
    }
    queues_.clear();
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
    return AllocateShared(AllocationOptions {bytes, alignment,
                                             config_.coherence_granule_bytes});
}

Result<GlobalPointer> LoomMemRuntime::AllocateShared(const AllocationOptions& options) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before allocation");
    }
    const auto result = allocator_->Allocate(options);
    if (!result.ok())
        return result.status();
    const auto token_status = token_service_->RegisterAllocation(result.value());
    if (!token_status.ok())
        return token_status;
    return result.value();
}

Status LoomMemRuntime::FreeShared(GlobalPointer gptr) {
    if (!initialized_)
        return Status::FailedPrecondition("runtime must be initialized before free");
    // One caller drives a transaction on this host; timeout leaves it resumable.
    std::unique_lock<std::mutex> coordinator(retirement_mutex_, std::try_to_lock);
    if (!coordinator.owns_lock()) return Status::Unavailable("another local free is in progress");
    if (config_.host_count > 1 && (queue_poller_ == nullptr || !queue_poller_->running()))
        return Status::FailedPrecondition("multi-host retirement requires a running poller on every host");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.bootstrap_timeout_ms);
    bool began = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!began) {
            const auto transaction = allocator_->BeginRetire(gptr, config_.host_count);
            if (!transaction.ok()) {
                if (transaction.status().code() != StatusCode::kUnavailable) return transaction.status();
                std::this_thread::yield();
                continue;
            }
            began = true;
        }
        // With no inbound channels there can be no message handler in flight.
        if (config_.host_count == 1) {
            const auto status = ProgressRetirement();
            if (!status.ok()) return status;
        }
        const auto transaction = allocator_->Retirement();
        if (transaction.phase == RetirementPhase::kReclaimable) {
            const auto status = allocator_->Free(gptr);
            if (status.code() != StatusCode::kUnavailable) return status;
        }
        if (queue_poller_ && !queue_poller_->running())
            return Status::Unavailable("retirement retained after poller failure");
        std::this_thread::yield();
    }
    return Status::Unavailable(began
        ? "retirement incomplete; object remains inaccessible and storage is not reusable"
        : "timed out waiting for the shared retirement coordinator");
}

Status LoomMemRuntime::ProgressRetirement() {
    const auto outbound = token_service_->ProgressOutbound();
    if (!outbound.ok()) return outbound;
    const auto transaction = allocator_->Retirement();
    if (transaction.phase == RetirementPhase::kIdle || transaction.phase == RetirementPhase::kReclaimable ||
        allocator_->RetirementAcknowledged(transaction, config_.local_host_id))
        return Status::Ok();
    std::array<std::uint64_t, kMaxHosts> cursors {};
    if (transaction.phase == RetirementPhase::kClosing) {
        if (!token_service_->CloseObject(transaction.object)) return Status::Ok();
        for (HostId host = 0; host < config_.host_count; ++host)
            if (host != config_.local_host_id)
                cursors[host] = queues_[config_.local_host_id][host]->PublishedSequence();
    } else if (transaction.phase == RetirementPhase::kDraining) {
        // Called only after ScanOnce handlers return, never by the freeing
        // thread in a multi-host runtime. A popped packet alone is insufficient.
        for (HostId host = 0; host < config_.host_count; ++host)
            if (host != config_.local_host_id)
                cursors[host] = queues_[host][config_.local_host_id]->ConsumedSequence();
    } else if (transaction.phase == RetirementPhase::kCleaning) {
        const auto descriptor = allocator_->MutableDescriptor(transaction.object, true);
        if (!descriptor.ok()) return descriptor.status();
        token_service_->ForgetAllocation(transaction.object, descriptor.value()->coherence_metadata_offset,
                                         descriptor.value()->coherence_block_count);
        std::lock_guard<std::mutex> lock(replicas_mutex_);
        InvalidateObjectLocked(transaction.object.offset);
    }
    const auto status = allocator_->AcknowledgeRetirement(transaction, config_.local_host_id, cursors);
    return status.code() == StatusCode::kUnavailable ? Status::Ok() : status;
}

Result<ObjectReference> LoomMemRuntime::AcquireObjectReference(GlobalPointer gptr) {
    if (!initialized_)
        return Status::FailedPrecondition("object references require an initialized shared runtime");
    auto* allocator = allocator_.get();
    const auto descriptor = allocator->AcquireReference(gptr, config_.local_host_id);
    if (!descriptor.ok())
        return descriptor.status();
    std::shared_ptr<void> guard(descriptor.value(), [allocator, host = config_.local_host_id](void* p) {
        allocator->ReleaseReference(static_cast<AllocationDescriptor*>(p), host);
    });
    return ObjectReference {gptr, std::move(guard)};
}

Result<void*> LoomMemRuntime::ResolveLocal(const GlobalPointer& gptr) const {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomMem runtime must be initialized before address resolution");
    }
    if (gptr.region_id != 0 || gptr.offset < layout_.shared_data.offset ||
        gptr.offset >= layout_.shared_data.offset + layout_.shared_data.bytes) {
        return Status::InvalidArgument("global pointer is outside the mapped shared region");
    }
    const auto allocation = DescribeSharedAllocation(gptr);
    if (!allocation.ok())
        return allocation.status();
    auto* base = static_cast<std::byte*>(region_mapper_.base());
    return base + gptr.offset;
}

Result<AllocationInfo> LoomMemRuntime::DescribeSharedAllocation(GlobalPointer gptr) const {
    if (!initialized_)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto* allocator = allocator_.get();
    return allocator->Describe(gptr);
}

Result<HostId> LoomMemRuntime::ResolveOwningHost(GlobalPointer gptr) const {
    if (!initialized_)
        return Status::FailedPrecondition("runtime is not initialized");
    const auto* allocator = allocator_.get();
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

Result<std::vector<BlockLocality>> LoomMemRuntime::QueryLocality(const WorkingSetEntry& entry) const {
    if (!initialized_) return Status::FailedPrecondition("locality requires initialized LoomMem");
    auto reference = allocator_->AcquireReference(entry.object, config_.local_host_id);
    if (!reference.ok()) return reference.status();
    auto* descriptor = reference.value();
    auto guard = std::shared_ptr<void>(descriptor, [this](void* p) {
        allocator_->ReleaseReference(static_cast<AllocationDescriptor*>(p), config_.local_host_id);
    });
    if (entry.offset >= descriptor->bytes || entry.bytes > descriptor->bytes - entry.offset)
        return Status::InvalidArgument("working set range outside allocation");
    const auto bytes = entry.bytes ? entry.bytes : descriptor->bytes - entry.offset;
    const auto granule = descriptor->coherence_block_bytes;
    std::vector<BlockLocality> result;
    for (auto i = entry.offset / granule; i <= (entry.offset + bytes - 1) / granule; ++i) {
        auto found = allocator_->MutableCoherenceBlock(entry.object, i, true);
        if (!found.ok()) return found.status();
        auto* block = found.value();
        bool sampled = false;
        for (unsigned retry = 0; retry < 32; ++retry) {
            const auto epoch = block->writeback_epoch.load(std::memory_order_acquire);
            if (epoch & 1) continue;
            BlockLocality sample{};
            sample.bytes = std::min(entry.offset + bytes, (i + 1) * granule) - std::max(entry.offset, i * granule);
            sample.version = block->version.load(std::memory_order_acquire);
            sample.token_owner = block->token_owner.load(std::memory_order_acquire);
            sample.last_writer = block->last_writer.load(std::memory_order_acquire);
            for (HostId host = 0; host < config_.host_count; ++host)
                if (block->replica_versions[host].load(std::memory_order_acquire) == sample.version + 1)
                    sample.current_replica_hosts |= 1ULL << host;
            if (epoch != block->writeback_epoch.load(std::memory_order_acquire)) continue;
            result.push_back(sample);
            sampled = true;
            break;
        }
        if (!sampled) return Status::Unavailable("locality changed during writeback; retry create");
    }
    return result;
}

Result<HostId> LoomMemRuntime::ResolvePreferredHost(const GlobalPointer& gptr) const {
    auto blocks = QueryLocality(WorkingSetEntry{gptr});
    if (!blocks.ok()) return blocks.status();
    std::array<std::uint64_t, kMaxHosts> owned_bytes{};
    for (const auto& block : blocks.value()) {
        if (block.token_owner >= config_.host_count)
            return Status::FailedPrecondition("invalid coherence token owner");
        owned_bytes[block.token_owner] += block.bytes;
    }
    return static_cast<HostId>(std::max_element(owned_bytes.begin(), owned_bytes.end()) - owned_bytes.begin());
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
    // Local token arbitration is synchronous, and single-host retirement is
    // driven by FreeShared. There are no transport channels to poll.
    if (config_.host_count == 1)
        return Status::Ok();
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
            message.header.kind == MessageKind::kTokenCancelAck) {
            if (token_service_ == nullptr)
                return Status::FailedPrecondition("token message received without a token service");
            return token_service_->HandleMessage(message);
        }
        if (!application_handler)
            return Status::InvalidArgument("no handler is registered for this queue message kind");
        return application_handler(std::move(message));
    };
    queue_poller_ = std::make_unique<QueuePoller>(config_.local_host_id, std::move(inbound_queues),
                                                  std::move(dispatch), options, [this] { return ProgressRetirement(); });
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
    if (config_.host_count > 1 && (queue_poller_ == nullptr || !queue_poller_->running()))
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
    std::shared_lock<std::shared_mutex> boundary(replica_boundary_mutex_);
    auto mutex = LocalObjectMutex(lease.object.offset);
    // Finish an odd raw-write epoch before waiting for a same-object reader:
    // that reader may itself be waiting for this writeback to become stable.
    const auto status = token_service_->Release(lease);
    std::lock_guard<std::mutex> object_lock(*mutex);
    if (status.ok()) {
        std::lock_guard<std::mutex> lock(replicas_mutex_);
        const auto object = cached_objects_.find(lease.object.offset);
        if (object != cached_objects_.end()) {
            const auto key = object->second.info.coherence_metadata_offset +
                             lease.block_index * sizeof(CoherenceBlockDescriptor);
            for (auto* index : {&replicas_, &old_replicas_}) {
                const auto entry = index->find(key);
                if (entry != index->end()) EraseReplicaLocked(*index, entry);
            }
        }
    }
    return status;
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
    return replicas_.size() + old_replicas_.size();
}

std::size_t LoomMemRuntime::cached_replica_bytes() const {
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    return cached_replica_bytes_;
}

std::size_t LoomMemRuntime::pending_token_request_count() const {
    return token_service_ == nullptr ? 0 : token_service_->pending_request_count();
}

std::shared_ptr<std::mutex> LoomMemRuntime::LocalObjectMutex(std::uint64_t offset) {
    std::lock_guard<std::mutex> lock(object_mutexes_mutex_);
    // Weak entries never own an object or CXL storage; bound dead discovery keys.
    if (object_mutexes_.size() > 1024) {
        for (auto it = object_mutexes_.begin(); it != object_mutexes_.end();)
            if (it->second.expired()) it = object_mutexes_.erase(it); else ++it;
    }
    auto& weak = object_mutexes_[offset];
    auto mutex = weak.lock();
    if (!mutex) { mutex = std::make_shared<std::mutex>(); weak = mutex; }
    return mutex;
}

void LoomMemRuntime::EraseReplicaLocked(ReplicaIndex& index, ReplicaIndex::iterator entry) {
    auto object = cached_objects_.find(entry->second.object_offset);
    auto block = allocator_->MutableCoherenceBlock(
        object->second.info.gptr, entry->second.block_index, true);
    if (block.ok()) block.value()->replica_versions[config_.local_host_id].store(0, std::memory_order_release);
    cached_replica_bytes_ -= entry->second.storage->size();
    index.erase(entry);
    if (--object->second.blocks == 0) {
        // Membership itself pins the descriptor until retirement cleaning.
        object->second.descriptor->replica_hosts.fetch_and(~(1ULL << config_.local_host_id),
                                                          std::memory_order_acq_rel);
        cached_objects_.erase(object);
    }
}

void LoomMemRuntime::InvalidateObjectLocked(std::uint64_t offset) {
    for (auto* index : {&replicas_, &old_replicas_}) {
        for (auto it = index->begin(); it != index->end();) {
            if (it->second.object_offset == offset) {
                auto victim = it++;
                EraseReplicaLocked(*index, victim);
            } else ++it;
        }
    }
}

Status LoomMemRuntime::InvalidateReadCache(GlobalPointer object) {
    if (!initialized_) return Status::FailedPrecondition("invalidation requires initialized LoomMem");
    if (object.region_id != 0) return Status::InvalidArgument("invalid region id");
    std::shared_lock<std::shared_mutex> boundary(replica_boundary_mutex_);
    auto mutex = LocalObjectMutex(object.offset);
    std::lock_guard<std::mutex> object_lock(*mutex);
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    InvalidateObjectLocked(object.offset);
    return Status::Ok();
}

void LoomMemRuntime::EvictReplicasLocked() {
    while (replicas_.size() + old_replicas_.size() > config_.replica_cache_capacity_entries ||
           cached_replica_bytes_ > config_.replica_cache_capacity_bytes) {
        ReplicaIndex* victim_index = nullptr;
        ReplicaIndex::iterator victim;
        for (auto* index : {&replicas_, &old_replicas_}) {
            for (auto candidate = index->begin(); candidate != index->end(); ++candidate) {
                if (!victim_index || candidate->second.last_access < victim->second.last_access) {
                    victim_index = index;
                    victim = candidate;
                }
            }
        }
        if (!victim_index) break;
        EraseReplicaLocked(*victim_index, victim);
    }
}

void LoomMemRuntime::CacheReplicaLocked(std::uint64_t cache_key, const AllocationInfo& info,
                                         AllocationDescriptor* descriptor, std::uint64_t block_index,
                                         Version version, std::shared_ptr<const std::vector<std::byte>> storage) {
    auto object = cached_objects_.find(info.gptr.offset);
    if (object == cached_objects_.end()) {
        object = cached_objects_.emplace(info.gptr.offset, CachedObject {info, descriptor, 0}).first;
        descriptor->replica_hosts.fetch_or(1ULL << config_.local_host_id, std::memory_order_acq_rel);
    }
    // Account for the replacement before erasing old/current entries so a host
    // retaining a replica never drops and re-adds its shared membership.
    ++object->second.blocks;
    for (auto* index : {&replicas_, &old_replicas_}) {
        const auto existing = index->find(cache_key);
        if (existing != index->end()) EraseReplicaLocked(*index, existing);
    }
    cached_replica_bytes_ += storage->size();
    replicas_.emplace(cache_key, CachedReplica {info.gptr.offset, block_index, version,
                                                std::move(storage), ++replica_access_clock_});
    auto block = allocator_->MutableCoherenceBlock(info.gptr, block_index, true);
    if (block.ok()) block.value()->replica_versions[config_.local_host_id].store(version + 1, std::memory_order_release);
    EvictReplicasLocked();
}

Result<ReadSnapshot> LoomMemRuntime::AcquireReadSnapshot(GlobalPointer object, std::uint64_t timeout_ms) {
    return ReadRange(object, 0, 0, timeout_ms, true);
}

Result<ReadSnapshot> LoomMemRuntime::AcquireReadRange(GlobalPointer object, std::uint64_t offset,
                                                      std::uint64_t bytes, std::uint64_t timeout_ms) {
    return ReadRange(object, offset, bytes, timeout_ms, false);
}

Result<ReadSnapshot> LoomMemRuntime::ReadRange(GlobalPointer object, std::uint64_t offset,
                                               std::uint64_t bytes, std::uint64_t timeout_ms, bool full_object) {
    if (!initialized_ || timeout_ms == 0)
        return Status::FailedPrecondition("read ranges require an initialized shared runtime and timeout");
    if (object.region_id != 0) return Status::InvalidArgument("invalid region id");
    std::shared_lock<std::shared_mutex> boundary(replica_boundary_mutex_);
    auto mutex = LocalObjectMutex(object.offset);
    std::lock_guard<std::mutex> object_lock(*mutex);
    auto* allocator = allocator_.get();
    AllocationInfo info {};
    AllocationDescriptor* descriptor = nullptr;
    std::shared_ptr<void> operation_guard;
    auto admit = [&]() -> Status {
        if (operation_guard) return Status::Ok();
        const auto result = allocator->AcquireReference(object, config_.local_host_id);
        if (!result.ok()) return result.status();
        descriptor = result.value();
        operation_guard = std::shared_ptr<void>(descriptor, [allocator, host = config_.local_host_id](void* p) {
            allocator->ReleaseReference(static_cast<AllocationDescriptor*>(p), host);
        });
        if (info.bytes != 0 && (info.bytes != descriptor->bytes ||
            info.coherence_block_bytes != descriptor->coherence_block_bytes ||
            info.coherence_metadata_offset != descriptor->coherence_metadata_offset))
            return Status::Unavailable("allocation changed during local cache lookup; retry the read");
        info = AllocationInfo {object, descriptor->bytes, descriptor->alignment, descriptor->owner_host,
                               descriptor->coherence_block_bytes, descriptor->coherence_block_count,
                               descriptor->coherence_metadata_offset};
        return Status::Ok();
    };
    {
        std::lock_guard<std::mutex> lock(replicas_mutex_);
        const auto cached = cached_objects_.find(object.offset);
        if (cached != cached_objects_.end()) info = cached->second.info;
    }
    if (info.bytes == 0) {
        const auto status = admit();
        if (!status.ok()) return status;
    }
    if (full_object) bytes = info.bytes;
    if (bytes == 0 || offset > info.bytes || bytes > info.bytes - offset)
        return Status::InvalidArgument("read range is empty or outside the allocation");
    const auto first_block = offset / info.coherence_block_bytes;
    const auto last_block = (offset + bytes - 1) / info.coherence_block_bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto assembled = std::make_shared<std::vector<std::byte>>(bytes);
    std::shared_ptr<const std::vector<std::byte>> single_block_replica;
    std::vector<Version> versions;
    versions.reserve(last_block - first_block + 1);
    for (std::uint64_t index = first_block; index <= last_block; ++index) {
        const auto block_start = index * info.coherence_block_bytes;
        const auto block_bytes = std::min(info.coherence_block_bytes, info.bytes - block_start);
        const auto cache_key = info.coherence_metadata_offset + index * sizeof(CoherenceBlockDescriptor);
        std::shared_ptr<const std::vector<std::byte>> replica;
        Version accepted_version = 0;
        {
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            const auto cached = replicas_.find(cache_key);
            if (cached != replicas_.end() && cached->second.object_offset == object.offset) {
                replica = cached->second.storage;
                accepted_version = cached->second.version;
                cached->second.last_access = ++replica_access_clock_;
            }
        }
        if (!replica) {
            const auto status = admit();
            if (!status.ok()) return status;
            const auto block_result = allocator->MutableCoherenceBlock(object, index, true);
            if (!block_result.ok()) return block_result.status();
            auto* block = block_result.value();
            while (std::chrono::steady_clock::now() < deadline) {
                auto metadata_status = AcquireData(block, sizeof(*block), VisibilityMode::kReleaseAcquire);
                if (!metadata_status.ok()) return metadata_status;
                const auto epoch_before = block->writeback_epoch.load(std::memory_order_acquire);
                if (epoch_before & 1U) { std::this_thread::yield(); continue; }
                const auto version_before = block->version.load(std::memory_order_acquire);
                {
                    std::lock_guard<std::mutex> lock(replicas_mutex_);
                    const auto old = old_replicas_.find(cache_key);
                    if (old != old_replicas_.end() && old->second.version == version_before)
                        replica = old->second.storage;
                }
                if (!replica) {
                    auto refreshed = std::make_shared<std::vector<std::byte>>(block_bytes);
                    const auto data_status = AcquireData(static_cast<std::byte*>(region_mapper_.base()) +
                        object.offset + block_start, block_bytes, VisibilityMode::kReleaseAcquire);
                    if (!data_status.ok()) return data_status;
                    std::memcpy(refreshed->data(), static_cast<std::byte*>(region_mapper_.base()) +
                                object.offset + block_start, block_bytes);
                    replica = std::move(refreshed);
                }
                metadata_status = AcquireData(block, sizeof(*block), VisibilityMode::kReleaseAcquire);
                if (!metadata_status.ok()) return metadata_status;
                const auto epoch_after = block->writeback_epoch.load(std::memory_order_acquire);
                const auto version_after = block->version.load(std::memory_order_acquire);
                if (epoch_before != epoch_after || (epoch_after & 1U) || version_before != version_after) {
                    replica.reset();
                    std::this_thread::yield();
                    continue;
                }
                accepted_version = version_after;
                std::lock_guard<std::mutex> lock(replicas_mutex_);
                CacheReplicaLocked(cache_key, info, descriptor, index, accepted_version, replica);
                break;
            }
        }
        if (!replica) return Status::Unavailable("timed out waiting for a stable readable block version");
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
    return ReadSnapshot {object, offset, std::move(versions), std::move(result_storage)};
}

Result<WriteBuffer> LoomMemRuntime::AcquireWriteBuffer(GlobalPointer object, std::uint64_t timeout_ms) {
    const auto reference = AcquireObjectReference(object);
    if (!reference.ok()) return reference.status();
    const auto allocation = DescribeSharedAllocation(object);
    if (!allocation.ok())
        return allocation.status();
    return AcquireWriteRange(object, 0, allocation.value().bytes, timeout_ms);
}

Result<WriteBuffer> LoomMemRuntime::AcquireWriteRange(GlobalPointer object, std::uint64_t offset,
                                                       std::uint64_t bytes, std::uint64_t timeout_ms) {
    if (!initialized_ || token_service_ == nullptr || timeout_ms == 0)
        return Status::FailedPrecondition("write buffers require an initialized shared runtime");
    // Buffered writers do not touch shared bytes until release, so holding the
    // token alone must not make the last committed version unreadable.
    auto* shared_allocator = allocator_.get();
    const auto descriptor_result = shared_allocator->AcquireReference(object, config_.local_host_id);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    std::shared_ptr<void> reference_guard(
        descriptor, [shared_allocator, host = config_.local_host_id](void* p) {
            shared_allocator->ReleaseReference(static_cast<AllocationDescriptor*>(p), host);
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
    return WriteBuffer {leases.front(), leases, offset, std::move(storage), std::move(reference_guard)};
}

Status LoomMemRuntime::ReleaseWriteBuffer(const WriteBuffer& write) {
    if (!initialized_ || write.storage == nullptr || !write.reference_guard)
        return Status::FailedPrecondition("write buffer requires an initialized runtime and storage");
    std::shared_lock<std::shared_mutex> boundary(replica_boundary_mutex_);
    auto mutex = LocalObjectMutex(write.lease.object.offset);
    std::lock_guard<std::mutex> object_lock(*mutex);
    auto* shared_allocator = allocator_.get();
    const auto descriptor_result = shared_allocator->MutableDescriptor(write.lease.object, true);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    const auto& leases = write.leases.empty() ? std::vector<TokenLease> {write.lease} : write.leases;
    if (write.offset > descriptor->bytes ||
        write.storage->size() > descriptor->bytes - write.offset)
        return Status::FailedPrecondition("write buffer does not match the shared allocation");
    for (std::size_t lease_index = 0; lease_index < leases.size(); ++lease_index) {
        const auto& lease = leases[lease_index];
        const auto begin_status = token_service_->BeginWriteback(lease);
        if (!begin_status.ok()) {
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
        const auto release_status = token_service_->Release(lease);
        {
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            const auto key = descriptor->coherence_metadata_offset + lease.block_index * sizeof(CoherenceBlockDescriptor);
            for (auto* index : {&replicas_, &old_replicas_}) {
                const auto entry = index->find(key);
                if (entry != index->end()) EraseReplicaLocked(*index, entry);
            }
        }
        if (!release_status.ok()) {
            for (std::size_t remaining = lease_index + 1; remaining < leases.size(); ++remaining)
                token_service_->Release(leases[remaining], false);
            return release_status;
        }
    }
    write.reference_guard.reset();
    return Status::Ok();
}

Status LoomMemRuntime::AbortWriteBuffer(const WriteBuffer& write) {
    if (!initialized_ || token_service_ == nullptr || write.storage == nullptr || !write.reference_guard)
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

Status LoomMemRuntime::StageWriteBuffer(WriteBuffer* write) {
    if (!initialized_ || !token_service_ || !write || !write->storage || !write->reference_guard)
        return Status::FailedPrecondition("stage requires an active owned write buffer");
    if (write->storage.use_count() != 1 || write->reference_guard.use_count() != 1)
        return Status::FailedPrecondition("staging requires exclusive write buffer ownership");
    std::lock_guard<std::mutex> lock(staged_mutex_);
    staged_writes_[CurrentExecutionContextId()].push_back(std::move(*write));
    *write = WriteBuffer{};
    return Status::Ok();
}

std::size_t LoomMemRuntime::staged_write_count() const {
    std::lock_guard<std::mutex> lock(staged_mutex_);
    std::size_t count = 0;
    for (const auto& item : staged_writes_) count += item.second.size();
    return count;
}

Status LoomMemRuntime::SynchronizeRelease() {
    if (!initialized_) return Status::FailedPrecondition("release requires initialized LoomMem");
    std::vector<WriteBuffer> writes;
    {
        std::lock_guard<std::mutex> lock(staged_mutex_);
        auto it = staged_writes_.find(CurrentExecutionContextId());
        if (it != staged_writes_.end()) {
            writes = std::move(it->second);
            staged_writes_.erase(it);
        }
    }
    Status result = Status::Ok();
    for (auto& write : writes) {
        if (result.ok()) result = ReleaseWriteBuffer(write);
        if (!result.ok()) AbortWriteBuffer(write);
    }
    std::atomic_thread_fence(std::memory_order_release);
    return result;
}

Status LoomMemRuntime::SynchronizeAcquire() {
    if (!initialized_) return Status::FailedPrecondition("acquire requires initialized LoomMem");
    std::unique_lock<std::shared_mutex> boundary(replica_boundary_mutex_);
    std::atomic_thread_fence(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    while (!old_replicas_.empty()) EraseReplicaLocked(old_replicas_, old_replicas_.begin());
    old_replicas_.swap(replicas_);
    // Current is empty. Old blocks are validated lazily after this boundary.
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
