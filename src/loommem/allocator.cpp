#include "cxloom/loommem/allocator.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

namespace cxloom::loommem {

namespace {

class ObjectLock {
  public:
    explicit ObjectLock(AllocationDescriptor* descriptor) : lock_(descriptor->access_lock) {
        std::uint32_t expected = 0;
        while (!lock_.compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
            expected = 0;
            std::this_thread::yield();
        }
    }
    ~ObjectLock() { lock_.store(0, std::memory_order_release); }
  private:
    std::atomic<std::uint32_t>& lock_;
};
struct SlotGuard {
    AllocationSlot* slot;
    ~SlotGuard() { slot->readers.fetch_sub(1, std::memory_order_release); }
};

std::uint64_t AlignUp(std::uint64_t value, std::size_t alignment) {
    const auto safe_alignment = std::max<std::size_t>(alignment, 1);
    if ((safe_alignment & (safe_alignment - 1)) != 0) {
        return 0;
    }
    const auto mask = static_cast<std::uint64_t>(safe_alignment - 1);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

std::uint16_t InitialBin(std::uint64_t bytes) {
    if (bytes <= 1) return 0;
    const auto order = static_cast<unsigned>(63 - __builtin_clzll(bytes));
    return static_cast<std::uint16_t>(std::min<unsigned>(order, kTlsfBinCount - 1));
}

}  // namespace

Status FormatSharedAllocator(AllocatorHeader* header, std::size_t allocator_region_bytes,
                             std::uint64_t shared_data_offset, std::uint64_t shared_data_bytes,
                             std::uint64_t coherence_offset, std::uint64_t coherence_bytes) {
    if (header == nullptr || allocator_region_bytes < sizeof(AllocatorHeader)) {
        return Status::InvalidArgument("allocator region is too small for its shared header");
    }
    const auto coherence_data_offset = AlignUp(coherence_offset + sizeof(CoherenceRegionHeader),
                                               alignof(CoherenceBlockDescriptor));
    if (shared_data_bytes <= sizeof(AllocationDescriptor) || coherence_data_offset < coherence_offset ||
        coherence_data_offset >= coherence_offset + coherence_bytes)
        return Status::InvalidArgument("shared data or coherence region is too small for allocations");
    header = new (header) AllocatorHeader();
    header->state.store(static_cast<std::uint32_t>(AllocatorState::kInitializing), std::memory_order_relaxed);
    header->magic = kAllocatorMagic;
    header->layout_version = kAllocatorLayoutVersion;
    header->header_bytes = sizeof(AllocatorHeader);
    header->shared_data_offset = shared_data_offset;
    header->shared_data_bytes = shared_data_bytes;
    header->extent_lock.store(0, std::memory_order_relaxed);
    header->extent_node_capacity = kMaxSharedExtentNodes;
    header->data_bins.fill(kInvalidExtentIndex);
    header->coherence_bins.fill(kInvalidExtentIndex);
    header->data_bin_bitmap = header->coherence_bin_bitmap = 0;
    header->free_extent_node_head = 2;
    header->data_free_head = 0;
    header->coherence_free_head = 1;
    for (std::uint32_t i = 2; i < kMaxSharedExtentNodes; ++i) {
        header->extent_nodes[i] = {};
        header->extent_nodes[i].next = i + 1 < kMaxSharedExtentNodes ? i + 1 : kInvalidExtentIndex;
    }
    header->extent_nodes[0] = {};
    header->extent_nodes[0].offset = shared_data_offset;
    header->extent_nodes[0].bytes = shared_data_bytes;
    header->extent_nodes[0].state = static_cast<std::uint32_t>(SharedExtentState::kFree);
    header->extent_nodes[1] = {};
    header->extent_nodes[1].offset = coherence_data_offset;
    header->extent_nodes[1].bytes = coherence_offset + coherence_bytes - coherence_data_offset;
    header->extent_nodes[1].state = static_cast<std::uint32_t>(SharedExtentState::kFree);
    const auto data_bin = InitialBin(shared_data_bytes);
    const auto coherence_bin = InitialBin(header->extent_nodes[1].bytes);
    header->extent_nodes[0].bin = data_bin;
    header->extent_nodes[1].bin = coherence_bin;
    header->data_bins[data_bin] = 0;
    header->coherence_bins[coherence_bin] = 1;
    header->data_bin_bitmap = (1ULL << data_bin);
    header->coherence_bin_bitmap = (1ULL << coherence_bin);
    header->state.store(static_cast<std::uint32_t>(AllocatorState::kReady), std::memory_order_release);
    return Status::Ok();
}

Status FormatCoherenceRegion(CoherenceRegionHeader* header, std::uint64_t region_offset,
                             std::uint64_t region_bytes) {
    if (header == nullptr || region_bytes < sizeof(CoherenceRegionHeader) + sizeof(CoherenceBlockDescriptor))
        return Status::InvalidArgument("coherence region is too small for block metadata");
    header = new (header) CoherenceRegionHeader();
    header->magic = kCoherenceRegionMagic;
    header->layout_version = kCoherenceRegionLayoutVersion;
    header->header_bytes = sizeof(CoherenceRegionHeader);
    header->region_offset = region_offset;
    header->region_bytes = region_bytes;
    return Status::Ok();
}

SharedExtentAllocator::SharedExtentAllocator(AllocatorHeader* header, void* region_base, std::size_t region_bytes,
                                         HostId local_host, std::uint64_t expected_data_offset,
                                         std::uint64_t expected_data_bytes, CoherenceRegionHeader* coherence_header,
                                         std::size_t default_block_bytes)
    : header_(header), region_base_(static_cast<std::byte*>(region_base)), region_bytes_(region_bytes),
      local_host_(local_host), expected_data_offset_(expected_data_offset), expected_data_bytes_(expected_data_bytes),
      coherence_header_(coherence_header),
      default_block_bytes_(default_block_bytes) {}

Status SharedExtentAllocator::Initialize() {
    if (header_ == nullptr || region_base_ == nullptr || header_->magic != kAllocatorMagic ||
        header_->layout_version != kAllocatorLayoutVersion || header_->header_bytes != sizeof(AllocatorHeader)) {
        return Status::FailedPrecondition("shared allocator header is incompatible");
    }
    if (static_cast<AllocatorState>(header_->state.load(std::memory_order_acquire)) != AllocatorState::kReady) {
        return Status::FailedPrecondition("shared allocator is not ready");
    }
    if (!header_->state.is_lock_free() || !header_->extent_lock.is_lock_free() ||
        !header_->allocations[0].object_offset.is_lock_free() || !header_->allocations[0].readers.is_lock_free()) {
        return Status::FailedPrecondition("shared allocator requires lock-free shared atomics");
    }
    if (header_->shared_data_offset != expected_data_offset_ || header_->shared_data_bytes != expected_data_bytes_ ||
        expected_data_offset_ > region_bytes_ || expected_data_bytes_ > region_bytes_ - expected_data_offset_) {
        return Status::FailedPrecondition("shared allocator layout does not match runtime configuration");
    }
    if (coherence_header_ != nullptr &&
        (coherence_header_->magic != kCoherenceRegionMagic ||
         coherence_header_->layout_version != kCoherenceRegionLayoutVersion ||
         coherence_header_->header_bytes != sizeof(CoherenceRegionHeader) ||
         coherence_header_->region_offset > region_bytes_ ||
         coherence_header_->region_bytes > region_bytes_ - coherence_header_->region_offset)) {
        return Status::FailedPrecondition("coherence metadata region is incompatible");
    }
    if (header_->extent_node_capacity != kMaxSharedExtentNodes ||
        header_->data_free_head >= kMaxSharedExtentNodes ||
        header_->coherence_free_head >= kMaxSharedExtentNodes)
        return Status::FailedPrecondition("shared extent metadata is incompatible");
    initialized_ = true;
    return Status::Ok();
}

Result<GlobalPointer> SharedExtentAllocator::Allocate(std::size_t bytes, std::size_t alignment) {
    return Allocate(AllocationOptions {bytes, alignment, default_block_bytes_});
}

Result<GlobalPointer> SharedExtentAllocator::Allocate(const AllocationOptions& options) {
    const auto bytes = options.bytes;
    const auto alignment = options.alignment;
    if (!initialized_)
        return Status::FailedPrecondition("shared allocator is not initialized");
    if (bytes == 0)
        return Status::InvalidArgument("allocation size must be non-zero");
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return Status::InvalidArgument("alignment must be a non-zero power of two");
    }
    const auto block_bytes = options.coherence_block_bytes == 0 ? default_block_bytes_ : options.coherence_block_bytes;
    if (block_bytes < 64 || (block_bytes & (block_bytes - 1)) != 0)
        return Status::InvalidArgument("coherence block size must be a power of two and at least 64 bytes");
    const auto block_count = 1 + (bytes - 1) / block_bytes;
    if (block_count > UINT64_MAX / sizeof(CoherenceBlockDescriptor))
        return Status::InvalidArgument("coherence block metadata size overflows");

    if (coherence_header_ == nullptr)
        return Status::FailedPrecondition("shared allocations require a coherence extent pool");
    if (bytes > UINT64_MAX - sizeof(AllocationDescriptor) - (alignment - 1))
        return Status::InvalidArgument("allocation extent size overflows");
    const auto data_extent_bytes = sizeof(AllocationDescriptor) + bytes + alignment - 1;
    const auto coherence_extent_bytes = block_count * sizeof(CoherenceBlockDescriptor);
    LockExtents();
    AllocationSlot* slot = nullptr;
    for (auto& candidate : header_->allocations) {
        if (candidate.object_offset.load(std::memory_order_relaxed) == 0) {
            std::uint32_t expected = 0;
            if (candidate.readers.compare_exchange_strong(expected, UINT32_MAX, std::memory_order_acquire)) {
                slot = &candidate;
                break;
            }
        }
    }
    if (!slot) { UnlockExtents(); return Status::Unavailable("allocation discovery slots exhausted"); }
    const auto data_extent = AllocateExtentLocked(&header_->data_free_head, data_extent_bytes,
                                                   alignof(AllocationDescriptor));
    if (!data_extent.ok()) {
        slot->readers.store(0, std::memory_order_release);
        UnlockExtents();
        return data_extent.status();
    }
    const auto coherence_extent = AllocateExtentLocked(&header_->coherence_free_head, coherence_extent_bytes,
                                                        alignof(CoherenceBlockDescriptor));
    if (!coherence_extent.ok()) {
        FreeExtentLocked(&header_->data_free_head, data_extent.value(), data_extent_bytes);
        slot->readers.store(0, std::memory_order_release);
        UnlockExtents();
        return coherence_extent.status();
    }
    // Publish discovery only after the entire descriptor is initialized.

    const auto object_offset = AlignUp(data_extent.value() + sizeof(AllocationDescriptor), alignment);
    auto* descriptor = reinterpret_cast<AllocationDescriptor*>(region_base_ + object_offset -
                                                                sizeof(AllocationDescriptor));
    new (descriptor) AllocationDescriptor();
    descriptor->magic = kAllocationMagic;
    descriptor->owner_host = local_host_;
    descriptor->object_offset = object_offset;
    descriptor->bytes = bytes;
    descriptor->alignment = alignment;
    descriptor->coherence_block_bytes = block_bytes;
    descriptor->coherence_block_count = block_count;
    descriptor->coherence_metadata_offset = coherence_extent.value();
    descriptor->data_extent_offset = data_extent.value();
    descriptor->data_extent_bytes = data_extent_bytes;
    descriptor->coherence_extent_bytes = coherence_extent_bytes;
    auto* blocks = reinterpret_cast<CoherenceBlockDescriptor*>(region_base_ + coherence_extent.value());
    for (std::uint64_t index = 0; index < block_count; ++index) {
        new (&blocks[index]) CoherenceBlockDescriptor();
        blocks[index].token_owner.store(local_host_, std::memory_order_relaxed);
        blocks[index].version.store(0, std::memory_order_relaxed);
        blocks[index].token_epoch.store(1, std::memory_order_relaxed);
        blocks[index].writeback_epoch.store(0, std::memory_order_relaxed);
    }
    descriptor->state.store(static_cast<std::uint32_t>(AllocationState::kAllocated), std::memory_order_release);
    slot->object_offset.store(object_offset, std::memory_order_release);
    slot->readers.store(0, std::memory_order_release);
    UnlockExtents();
    return GlobalPointer {0, object_offset};
}

Result<AllocationDescriptor*> SharedExtentAllocator::FindDescriptorLocked(GlobalPointer object,
                                                                            bool allow_retiring) const {
    if (!initialized_)
        return Status::FailedPrecondition("shared allocator is not initialized");
    const auto end = expected_data_offset_ + expected_data_bytes_;
    if (object.region_id != 0 || object.offset < expected_data_offset_ + sizeof(AllocationDescriptor) ||
        object.offset >= end || (object.offset - sizeof(AllocationDescriptor)) % alignof(AllocationDescriptor) != 0)
        return Status::InvalidArgument("global pointer is not an aligned allocation base");
    auto* descriptor = reinterpret_cast<AllocationDescriptor*>(region_base_ + object.offset - sizeof(AllocationDescriptor));
    const auto state = static_cast<AllocationState>(descriptor->state.load(std::memory_order_acquire));
    if ((state != AllocationState::kAllocated && !(allow_retiring && state == AllocationState::kRetiring)) ||
        descriptor->magic != kAllocationMagic || descriptor->object_offset != object.offset ||
        descriptor->bytes == 0 || descriptor->bytes > end - object.offset || descriptor->owner_host >= kMaxHosts)
        return Status::NotFound("global pointer is not an accessible allocation base");
    return descriptor;
}

Result<AllocationSlot*> SharedExtentAllocator::LockAllocationSlot(GlobalPointer object) const {
    if (!initialized_) return Status::FailedPrecondition("shared allocator is not initialized");
    if (object.region_id != 0 || object.offset == 0)
        return Status::InvalidArgument("invalid allocation address");
    for (auto& slot : header_->allocations) {
        if (slot.object_offset.load(std::memory_order_acquire) != object.offset) continue;
        auto readers = slot.readers.load(std::memory_order_relaxed);
        while (readers != UINT32_MAX && readers != UINT32_MAX - 1) {
            if (slot.readers.compare_exchange_weak(readers, readers + 1, std::memory_order_acquire)) {
                if (slot.object_offset.load(std::memory_order_acquire) == object.offset) return &slot;
                slot.readers.fetch_sub(1, std::memory_order_release);
                break;
            }
        }
    }
    return Status::NotFound("address has no accessible allocation slot");
}

Result<AllocationInfo> SharedExtentAllocator::Describe(GlobalPointer object) const {
    const auto slot = LockAllocationSlot(object);
    if (!slot.ok()) return slot.status();
    SlotGuard guard {slot.value()};
    const auto found = FindDescriptorLocked(object, false);
    if (!found.ok()) return found.status();
    ObjectLock lock(found.value());
    if (found.value()->state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(AllocationState::kAllocated))
        return Status::NotFound("object is retiring");
    const auto* d = found.value();
    return AllocationInfo {object, d->bytes, d->alignment, d->owner_host,
                           d->coherence_block_bytes, d->coherence_block_count, d->coherence_metadata_offset};
}

Result<AllocationDescriptor*> SharedExtentAllocator::MutableDescriptor(GlobalPointer object, bool allow_retiring) const {
    const auto slot = LockAllocationSlot(object);
    if (!slot.ok()) return slot.status();
    SlotGuard guard {slot.value()};
    return FindDescriptorLocked(object, allow_retiring);
}

Result<HostId> SharedExtentAllocator::OwningHost(GlobalPointer object) const {
    const auto info = Describe(object);
    return info.ok() ? Result<HostId>(info.value().owner_host) : Result<HostId>(info.status());
}

Result<AllocationDescriptor*> SharedExtentAllocator::AcquireReference(GlobalPointer object, HostId host,
                                                                      bool allow_retiring) const {
    if (host >= kMaxHosts) return Status::InvalidArgument("reference host is out of range");
    const auto slot = LockAllocationSlot(object);
    if (!slot.ok()) return slot.status();
    SlotGuard guard {slot.value()};
    const auto found = FindDescriptorLocked(object, allow_retiring);
    if (!found.ok()) return found.status();
    ObjectLock lock(found.value());
    const auto state = static_cast<AllocationState>(found.value()->state.load(std::memory_order_acquire));
    if (state != AllocationState::kAllocated &&
        !(allow_retiring && state == AllocationState::kRetiring))
        return Status::FailedPrecondition("object is retiring");
    if (found.value()->sealed_hosts.load(std::memory_order_relaxed) & (1ULL << host))
        return Status::FailedPrecondition("host has sealed this retiring object");
    found.value()->active_operations[host].fetch_add(1, std::memory_order_relaxed);
    return found;
}

Status SharedExtentAllocator::ReleaseReference(AllocationDescriptor* descriptor, HostId host) const {
    if (!descriptor || host >= kMaxHosts) return Status::InvalidArgument("invalid reference release");
    const auto previous = descriptor->active_operations[host].fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
        descriptor->active_operations[host].fetch_add(1, std::memory_order_relaxed);
        return Status::FailedPrecondition("object reference count underflow");
    }
    return Status::Ok();
}

Result<RetirementSnapshot> SharedExtentAllocator::BeginRetire(GlobalPointer object, std::uint16_t host_count) {
    if (host_count == 0 || host_count > kMaxHosts) return Status::InvalidArgument("invalid retirement participants");
    LockExtents();
    auto& r = header_->retirement;
    if (r.current.phase != RetirementPhase::kIdle) {
        const bool same = r.current.object == object && r.current.coordinator == local_host_;
        const auto snapshot = r.current;
        UnlockExtents();
        return same ? Result<RetirementSnapshot>(snapshot)
                    : Result<RetirementSnapshot>(Status::Unavailable("another retirement is in progress"));
    }
    const auto found = FindDescriptorLocked(object, false);
    if (!found.ok()) { UnlockExtents(); return found.status(); }
    if (found.value()->owner_host != local_host_) {
        UnlockExtents(); return Status::FailedPrecondition("only the allocation owner may retire it");
    }
    if (r.current.sequence == UINT64_MAX) {
        UnlockExtents(); return Status::Unavailable("retirement sequence exhausted");
    }
    r.current = {RetirementPhase::kClosing, object, r.current.sequence + 1, local_host_, host_count};
    r.closed = r.drained = r.cleaned = 0;
    {
        ObjectLock lock(found.value());
        found.value()->state.store(static_cast<std::uint32_t>(AllocationState::kRetiring), std::memory_order_release);
    }
    const auto snapshot = r.current;
    UnlockExtents();
    return snapshot;
}

RetirementSnapshot SharedExtentAllocator::Retirement() const {
    LockExtents();
    const auto snapshot = header_->retirement.current;
    UnlockExtents();
    return snapshot;
}

bool SharedExtentAllocator::IsRetiring(GlobalPointer object) const {
    const auto slot = LockAllocationSlot(object);
    if (!slot.ok()) return false;
    SlotGuard guard {slot.value()};
    const auto descriptor = FindDescriptorLocked(object, true);
    return descriptor.ok() && descriptor.value()->state.load(std::memory_order_acquire) ==
                                 static_cast<std::uint32_t>(AllocationState::kRetiring);
}

bool SharedExtentAllocator::RetirementAcknowledged(const RetirementSnapshot& t, HostId host) const {
    LockExtents();
    const auto& r = header_->retirement;
    const auto mask = t.phase == RetirementPhase::kClosing ? r.closed :
                      t.phase == RetirementPhase::kDraining ? r.drained : r.cleaned;
    const bool done = r.current.sequence != t.sequence || r.current.phase != t.phase || (mask & (1ULL << host));
    UnlockExtents();
    return done;
}

Status SharedExtentAllocator::AcknowledgeRetirement(const RetirementSnapshot& t, HostId host,
                                                    const std::array<std::uint64_t, kMaxHosts>& cursors) {
    LockExtents();
    auto& r = header_->retirement;
    if (r.current.sequence != t.sequence || r.current.phase != t.phase || host >= t.host_count) {
        UnlockExtents(); return Status::FailedPrecondition("retirement phase changed");
    }
    const auto bit = 1ULL << host;
    const auto all = t.host_count == 64 ? UINT64_MAX : (1ULL << t.host_count) - 1;
    if (t.phase == RetirementPhase::kClosing) {
        const auto descriptor = FindDescriptorLocked(t.object, true);
        if (!descriptor.ok()) { UnlockExtents(); return descriptor.status(); }
        ObjectLock lock(descriptor.value());
        if (descriptor.value()->active_operations[host].load(std::memory_order_acquire) != 0) {
            UnlockExtents(); return Status::Unavailable("host still has active CXL operations");
        }
        descriptor.value()->sealed_hosts.fetch_or(bit, std::memory_order_release);
        r.watermarks[host] = cursors;
        r.closed |= bit;
        if (r.closed == all) r.current.phase = RetirementPhase::kDraining;
    } else if (t.phase == RetirementPhase::kDraining) {
        for (HostId producer = 0; producer < t.host_count; ++producer) {
            if (producer != host && cursors[producer] < r.watermarks[producer][host]) {
                UnlockExtents(); return Status::Unavailable("inbound callbacks have not reached the retirement watermark");
            }
        }
        r.drained |= bit;
        if (r.drained == all) r.current.phase = RetirementPhase::kCleaning;
    } else if (t.phase == RetirementPhase::kCleaning) {
        r.cleaned |= bit;
        if (r.cleaned == all) r.current.phase = RetirementPhase::kReclaimable;
    }
    UnlockExtents();
    return Status::Ok();
}

Result<CoherenceBlockDescriptor*> SharedExtentAllocator::MutableCoherenceBlock(GlobalPointer gptr,
                                                                              std::uint64_t block_index,
                                                                              bool allow_retiring) const {
    const auto descriptor = MutableDescriptor(gptr, allow_retiring);
    if (!descriptor.ok())
        return descriptor.status();
    if (block_index >= descriptor.value()->coherence_block_count)
        return Status::InvalidArgument("coherence block index is out of range");
    if (coherence_header_ == nullptr || descriptor.value()->coherence_metadata_offset == 0)
        return Status::FailedPrecondition("allocation has no coherence block sidecar metadata");
    const auto offset = descriptor.value()->coherence_metadata_offset +
                        block_index * sizeof(CoherenceBlockDescriptor);
    const auto coherence_end = coherence_header_->region_offset + coherence_header_->region_bytes;
    if (offset < coherence_header_->region_offset || offset > coherence_end ||
        sizeof(CoherenceBlockDescriptor) > coherence_end - offset)
        return Status::FailedPrecondition("coherence block metadata is outside its shared region");
    return reinterpret_cast<CoherenceBlockDescriptor*>(region_base_ + offset);
}


void SharedExtentAllocator::LockExtents() const {
    std::uint32_t expected = 0;
    while (!header_->extent_lock.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                                       std::memory_order_relaxed)) {
        expected = 0;
    }
}

void SharedExtentAllocator::UnlockExtents() const {
    header_->extent_lock.store(0, std::memory_order_release);
}

std::uint16_t SharedExtentAllocator::BinForBytes(std::uint64_t bytes) const {
    if (bytes <= 1) return 0;
    const auto order = static_cast<unsigned>(63 - __builtin_clzll(bytes));
    return static_cast<std::uint16_t>(std::min<unsigned>(order, kTlsfBinCount - 1));
}

void SharedExtentAllocator::InsertBinLocked(std::uint32_t* head, std::uint64_t* bitmap,
                                            std::uint32_t index) const {
    auto& node = header_->extent_nodes[index];
    const auto bin = BinForBytes(node.bytes);
    node.bin = bin;
    node.bin_previous = kInvalidExtentIndex;
    node.bin_next = head[bin];
    if (node.bin_next != kInvalidExtentIndex) header_->extent_nodes[node.bin_next].bin_previous = index;
    head[bin] = index;
    *bitmap |= (1ULL << bin);
}

void SharedExtentAllocator::RemoveBinLocked(std::uint32_t* head, std::uint64_t* bitmap,
                                            std::uint32_t index) const {
    auto& node = header_->extent_nodes[index];
    const auto bin = node.bin;
    if (node.bin_previous == kInvalidExtentIndex) head[bin] = node.bin_next;
    else header_->extent_nodes[node.bin_previous].bin_next = node.bin_next;
    if (node.bin_next != kInvalidExtentIndex) header_->extent_nodes[node.bin_next].bin_previous = node.bin_previous;
    if (head[bin] == kInvalidExtentIndex) *bitmap &= ~(1ULL << bin);
    node.bin_next = node.bin_previous = kInvalidExtentIndex;
}

Result<std::uint32_t> SharedExtentAllocator::ReserveExtentNodeLocked() const {
    const auto index = header_->free_extent_node_head;
    if (index == kInvalidExtentIndex) return Status::Unavailable("shared extent metadata is exhausted");
    header_->free_extent_node_head = header_->extent_nodes[index].next;
    header_->extent_nodes[index] = {};
    return index;
}

Result<std::uint64_t> SharedExtentAllocator::AllocateExtentLocked(std::uint32_t* head, std::uint64_t bytes,
                                                                std::size_t alignment) const {
    auto* bins = head == &header_->data_free_head ? header_->data_bins.data() : header_->coherence_bins.data();
    auto* bitmap = head == &header_->data_free_head ? &header_->data_bin_bitmap : &header_->coherence_bin_bitmap;
    const auto start = BinForBytes(bytes);
    const auto available = *bitmap & (~0ULL << start);
    if (available == 0) return Status::Unavailable("shared extent pool is exhausted");
    auto index = static_cast<std::uint32_t>(__builtin_ctzll(available));
    for (; index < kTlsfBinCount; ++index) {
      if ((available & (1ULL << index)) == 0) continue;
      for (auto node_index = bins[index]; node_index != kInvalidExtentIndex;
           node_index = header_->extent_nodes[node_index].bin_next) {
        auto& node = header_->extent_nodes[node_index];
        const auto allocated_offset = AlignUp(node.offset, alignment);
        if (allocated_offset < node.offset || allocated_offset > node.offset + node.bytes ||
            bytes > node.offset + node.bytes - allocated_offset) continue;
        const auto prefix = allocated_offset - node.offset;
        const auto suffix_offset = allocated_offset + bytes;
        const auto suffix = node.offset + node.bytes - suffix_offset;
        const auto old_next = node.next;
        RemoveBinLocked(bins, bitmap, node_index);
        if (prefix != 0 && suffix != 0) {
            const auto reserved = ReserveExtentNodeLocked();
            if (!reserved.ok()) { InsertBinLocked(bins, bitmap, node_index); return reserved.status(); }
            auto& extra = header_->extent_nodes[reserved.value()];
            extra.offset = suffix_offset; extra.bytes = suffix; extra.state = static_cast<std::uint32_t>(SharedExtentState::kFree);
            extra.next = old_next; extra.previous = node_index;
            if (old_next != kInvalidExtentIndex) header_->extent_nodes[old_next].previous = reserved.value();
            node.bytes = prefix; node.next = reserved.value(); node.state = static_cast<std::uint32_t>(SharedExtentState::kFree);
            InsertBinLocked(bins, bitmap, node_index); InsertBinLocked(bins, bitmap, reserved.value());
        } else if (prefix != 0) {
            node.bytes = prefix; InsertBinLocked(bins, bitmap, node_index);
        } else if (suffix != 0) {
            node.offset = suffix_offset; node.bytes = suffix; InsertBinLocked(bins, bitmap, node_index);
        } else {
            if (node.previous != kInvalidExtentIndex) header_->extent_nodes[node.previous].next = old_next;
            else *head = old_next;
            if (old_next != kInvalidExtentIndex) header_->extent_nodes[old_next].previous = node.previous;
            node.state = static_cast<std::uint32_t>(SharedExtentState::kUnused);
            node.next = header_->free_extent_node_head; header_->free_extent_node_head = node_index;
        }
        return allocated_offset;
      }
    }
    return Status::Unavailable("shared extent pool is exhausted");
}

Status SharedExtentAllocator::FreeExtentLocked(std::uint32_t* head, std::uint64_t offset,
                                             std::uint64_t bytes) const {
    if (bytes == 0) return Status::InvalidArgument("cannot free an empty extent");
    auto* bins = head == &header_->data_free_head ? header_->data_bins.data() : header_->coherence_bins.data();
    auto* bitmap = head == &header_->data_free_head ? &header_->data_bin_bitmap : &header_->coherence_bin_bitmap;
    auto previous = kInvalidExtentIndex, next = *head;
    while (next != kInvalidExtentIndex && header_->extent_nodes[next].offset < offset) { previous = next; next = header_->extent_nodes[next].next; }
    if ((previous != kInvalidExtentIndex && header_->extent_nodes[previous].offset + header_->extent_nodes[previous].bytes > offset) ||
        (next != kInvalidExtentIndex && offset + bytes > header_->extent_nodes[next].offset)) return Status::FailedPrecondition("freed extent overlaps the free pool");
    if (previous != kInvalidExtentIndex && header_->extent_nodes[previous].offset + header_->extent_nodes[previous].bytes == offset) {
        RemoveBinLocked(bins, bitmap, previous); header_->extent_nodes[previous].bytes += bytes;
        if (next != kInvalidExtentIndex && offset + bytes == header_->extent_nodes[next].offset) { RemoveBinLocked(bins, bitmap, next); header_->extent_nodes[previous].bytes += header_->extent_nodes[next].bytes; header_->extent_nodes[previous].next = header_->extent_nodes[next].next; if (header_->extent_nodes[next].next != kInvalidExtentIndex) header_->extent_nodes[header_->extent_nodes[next].next].previous = previous; header_->extent_nodes[next].state = static_cast<std::uint32_t>(SharedExtentState::kUnused); header_->extent_nodes[next].next = header_->free_extent_node_head; header_->free_extent_node_head = next; }
        InsertBinLocked(bins, bitmap, previous); return Status::Ok();
    }
    if (next != kInvalidExtentIndex && offset + bytes == header_->extent_nodes[next].offset) { RemoveBinLocked(bins, bitmap, next); header_->extent_nodes[next].offset = offset; header_->extent_nodes[next].bytes += bytes; InsertBinLocked(bins, bitmap, next); return Status::Ok(); }
    const auto reserved = ReserveExtentNodeLocked(); if (!reserved.ok()) return reserved.status();
    auto& node = header_->extent_nodes[reserved.value()]; node.offset=offset; node.bytes=bytes; node.next=next; node.previous=previous; node.state=static_cast<std::uint32_t>(SharedExtentState::kFree);
    if (previous != kInvalidExtentIndex) header_->extent_nodes[previous].next=reserved.value(); else *head=reserved.value();
    if (next != kInvalidExtentIndex) header_->extent_nodes[next].previous=reserved.value(); InsertBinLocked(bins, bitmap, reserved.value()); return Status::Ok();
}

Status SharedExtentAllocator::Free(GlobalPointer gptr) {
    LockExtents();
    const auto& transaction = header_->retirement.current;
    if (transaction.phase != RetirementPhase::kReclaimable || !(transaction.object == gptr) ||
        transaction.coordinator != local_host_) {
        UnlockExtents(); return Status::FailedPrecondition("storage reclamation requires all-host retirement completion");
    }
    AllocationSlot* slot = nullptr;
    for (auto& candidate : header_->allocations)
        if (candidate.object_offset.load(std::memory_order_acquire) == gptr.offset) { slot = &candidate; break; }
    if (!slot) { UnlockExtents(); return Status::NotFound("allocation slot not found"); }
    std::uint32_t expected = 0;
    if (!slot->readers.compare_exchange_strong(expected, UINT32_MAX, std::memory_order_acquire)) {
        UnlockExtents(); return Status::Unavailable("descriptor discovery is still active");
    }
    const auto found = FindDescriptorLocked(gptr, true);
    if (!found.ok()) { slot->readers.store(0, std::memory_order_release); UnlockExtents(); return found.status(); }
    auto* descriptor = found.value();
    if (descriptor->replica_hosts.load(std::memory_order_acquire) != 0) {
        slot->readers.store(0, std::memory_order_release);
        UnlockExtents(); return Status::Unavailable("allocation still has replica holders");
    }
    for (const auto& active : descriptor->active_operations) {
        if (active.load(std::memory_order_acquire) != 0) {
            slot->readers.store(0, std::memory_order_release);
            UnlockExtents(); return Status::Unavailable("allocation still has active operations");
        }
    }
    const auto data_offset = descriptor->data_extent_offset;
    const auto data_bytes = descriptor->data_extent_bytes;
    const auto coherence_offset = descriptor->coherence_metadata_offset;
    const auto coherence_bytes = descriptor->coherence_extent_bytes;
    std::size_t available_nodes = 0;
    for (std::uint32_t index = 0; index < header_->extent_node_capacity; ++index)
        available_nodes += header_->extent_nodes[index].state ==
                           static_cast<std::uint32_t>(SharedExtentState::kUnused);
    if (available_nodes < 2) {
        slot->readers.store(0, std::memory_order_release);
        UnlockExtents();
        return Status::Unavailable("insufficient extent metadata to reclaim object atomically");
    }
    const auto data_status = FreeExtentLocked(&header_->data_free_head, data_offset, data_bytes);
    const auto coherence_status = data_status.ok()
                                      ? FreeExtentLocked(&header_->coherence_free_head, coherence_offset,
                                                         coherence_bytes)
                                      : data_status;
    if (data_status.ok() && coherence_status.ok()) {
        descriptor->state.store(static_cast<std::uint32_t>(AllocationState::kEmpty), std::memory_order_release);
        descriptor->magic = 0;
        slot->object_offset.store(0, std::memory_order_release);
        header_->retirement.current.phase = RetirementPhase::kIdle;
    }
    slot->readers.store(0, std::memory_order_release);
    UnlockExtents();
    return !data_status.ok() ? data_status : coherence_status;
}

}  // namespace cxloom::loommem
