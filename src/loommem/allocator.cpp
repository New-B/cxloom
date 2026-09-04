#include "cxloom/loommem/allocator.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace cxloom::loommem {

namespace {

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

std::uint64_t NextPowerOfTwo(std::uint64_t value) {
    if (value <= 1) {
        return 1;
    }
    --value;
    for (std::size_t shift = 1; shift < sizeof(value) * 8; shift <<= 1) {
        value |= value >> shift;
    }
    return value + 1;
}

}  // namespace

SlabExtentAllocator::SlabExtentAllocator(std::size_t shared_region_bytes) : shared_region_bytes_(shared_region_bytes) {}

Status SlabExtentAllocator::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    free_extents_.clear();
    slab_free_blocks_.clear();
    allocations_.clear();
    free_extents_.emplace(0, shared_region_bytes_);
    initialized_ = true;
    return Status::Ok();
}

Result<GlobalPointer> SlabExtentAllocator::Allocate(std::size_t bytes, std::size_t alignment) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Status::FailedPrecondition("allocator must be initialized before use");
    }
    if (bytes == 0) {
        return Status::InvalidArgument("allocation size must be non-zero");
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return Status::InvalidArgument("alignment must be a non-zero power of two");
    }

    const auto object_bytes = std::max<std::size_t>(bytes, alignment);
    if (object_bytes <= kMaxSlabObjectBytes) {
        return AllocateSlab(bytes, alignment);
    }
    return AllocateExtent(bytes, alignment);
}

Result<GlobalPointer> SlabExtentAllocator::AllocateSlab(std::size_t bytes, std::size_t alignment) {
    const auto object_bytes = NextPowerOfTwo(std::max<std::size_t>(bytes, alignment));
    auto& free_blocks = slab_free_blocks_[object_bytes];
    if (free_blocks.empty()) {
        const auto reserve_status = ReserveSlabPage(object_bytes);
        if (!reserve_status.ok()) {
            return reserve_status;
        }
    }

    const auto offset = free_blocks.back();
    free_blocks.pop_back();
    allocations_[offset] = AllocationRecord {AllocationKind::kSlab, object_bytes};
    return GlobalPointer {0, offset};
}

Result<GlobalPointer> SlabExtentAllocator::AllocateExtent(std::size_t bytes, std::size_t alignment) {
    const auto offset = ReserveExtent(bytes, alignment);
    if (!offset.ok()) {
        return offset.status();
    }
    allocations_[offset.value()] = AllocationRecord {AllocationKind::kExtent, bytes};
    return GlobalPointer {0, offset.value()};
}

Status SlabExtentAllocator::ReserveSlabPage(std::uint64_t object_bytes) {
    const auto page = ReserveExtent(kSlabPageBytes, kSlabPageBytes);
    if (!page.ok()) {
        return page.status();
    }

    auto& free_blocks = slab_free_blocks_[object_bytes];
    for (std::uint64_t offset = page.value(); offset < page.value() + kSlabPageBytes; offset += object_bytes) {
        free_blocks.push_back(offset);
    }
    return Status::Ok();
}

Result<std::uint64_t> SlabExtentAllocator::ReserveExtent(std::uint64_t bytes, std::size_t alignment) {
    for (auto it = free_extents_.begin(); it != free_extents_.end(); ++it) {
        const auto extent_offset = it->first;
        const auto extent_bytes = it->second;
        const auto aligned_offset = AlignUp(extent_offset, alignment);
        // Offset zero is the valid first address in the shared-data region.
        if (aligned_offset < extent_offset) {
            continue;
        }
        const auto prefix_bytes = aligned_offset - extent_offset;
        if (prefix_bytes > extent_bytes || bytes > extent_bytes - prefix_bytes) {
            continue;
        }

        const auto suffix_offset = aligned_offset + bytes;
        const auto suffix_bytes = extent_bytes - prefix_bytes - bytes;
        free_extents_.erase(it);
        if (prefix_bytes != 0) {
            free_extents_.emplace(extent_offset, prefix_bytes);
        }
        if (suffix_bytes != 0) {
            free_extents_.emplace(suffix_offset, suffix_bytes);
        }
        return aligned_offset;
    }
    return Status::Unavailable("shared region exhausted");
}

void SlabExtentAllocator::InsertFreeExtent(std::uint64_t offset, std::uint64_t bytes) {
    auto next = free_extents_.lower_bound(offset);
    if (next != free_extents_.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset) {
            offset = previous->first;
            bytes += previous->second;
            free_extents_.erase(previous);
        }
    }
    if (next != free_extents_.end() && offset + bytes == next->first) {
        bytes += next->second;
        free_extents_.erase(next);
    }
    free_extents_.emplace(offset, bytes);
}

Status SlabExtentAllocator::Free(GlobalPointer gptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Status::FailedPrecondition("allocator must be initialized before use");
    }
    if (gptr.region_id != 0) {
        return Status::InvalidArgument("allocator received an unknown region id");
    }

    const auto it = allocations_.find(gptr.offset);
    if (it == allocations_.end()) {
        return Status::NotFound("unknown or already freed global pointer");
    }

    const auto allocation = it->second;
    allocations_.erase(it);
    if (allocation.kind == AllocationKind::kSlab) {
        slab_free_blocks_[allocation.span_bytes].push_back(gptr.offset);
        return Status::Ok();
    }

    InsertFreeExtent(gptr.offset, allocation.span_bytes);
    return Status::Ok();
}

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
    header->next_allocation_id.store(1, std::memory_order_relaxed);
    header->extent_lock.store(0, std::memory_order_relaxed);
    header->extent_node_capacity = kMaxSharedExtentNodes;
    header->data_free_head = 0;
    header->coherence_free_head = 1;
    header->extent_nodes[0] = {shared_data_offset, shared_data_bytes, kInvalidExtentIndex,
                               static_cast<std::uint32_t>(SharedExtentState::kFree)};
    header->extent_nodes[1] = {coherence_data_offset, coherence_offset + coherence_bytes - coherence_data_offset,
                               kInvalidExtentIndex, static_cast<std::uint32_t>(SharedExtentState::kFree)};
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
                                         CoherenceGranularity default_granularity, std::size_t default_block_bytes)
    : header_(header), region_base_(static_cast<std::byte*>(region_base)), region_bytes_(region_bytes),
      local_host_(local_host), expected_data_offset_(expected_data_offset), expected_data_bytes_(expected_data_bytes),
      coherence_header_(coherence_header), default_granularity_(default_granularity),
      default_block_bytes_(default_block_bytes) {}

Status SharedExtentAllocator::Initialize() {
    if (header_ == nullptr || region_base_ == nullptr || header_->magic != kAllocatorMagic ||
        header_->layout_version != kAllocatorLayoutVersion || header_->header_bytes != sizeof(AllocatorHeader)) {
        return Status::FailedPrecondition("shared allocator header is incompatible");
    }
    if (static_cast<AllocatorState>(header_->state.load(std::memory_order_acquire)) != AllocatorState::kReady) {
        return Status::FailedPrecondition("shared allocator is not ready");
    }
    if (!header_->state.is_lock_free() || !header_->next_allocation_id.is_lock_free() ||
        !header_->extent_lock.is_lock_free()) {
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
    return Allocate(AllocationOptions {bytes, alignment, default_granularity_, default_block_bytes_});
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
    if (options.coherence_granularity != CoherenceGranularity::kObject &&
        options.coherence_granularity != CoherenceGranularity::kFixedBlock)
        return Status::InvalidArgument("allocation coherence granularity is invalid");

    std::uint64_t block_bytes = bytes;
    if (options.coherence_granularity == CoherenceGranularity::kFixedBlock) {
        block_bytes = options.coherence_block_bytes == 0 ? default_block_bytes_ : options.coherence_block_bytes;
        if (block_bytes < 64 || (block_bytes & (block_bytes - 1)) != 0)
            return Status::InvalidArgument("coherence block size must be a power of two and at least 64 bytes");
    }
    const auto block_count = 1 + (bytes - 1) / block_bytes;
    if (block_count > UINT64_MAX / sizeof(CoherenceBlockDescriptor))
        return Status::InvalidArgument("coherence block metadata size overflows");

    if (coherence_header_ == nullptr)
        return Status::FailedPrecondition("shared allocations require a coherence extent pool");
    if (bytes > UINT64_MAX - sizeof(AllocationDescriptor) - (alignment - 1))
        return Status::InvalidArgument("allocation extent size overflows");
    const auto data_extent_bytes = sizeof(AllocationDescriptor) + bytes + alignment - 1;
    const auto coherence_extent_bytes = block_count * sizeof(CoherenceBlockDescriptor);
    const auto allocation_id = header_->next_allocation_id.fetch_add(1, std::memory_order_relaxed);
    if (allocation_id == 0)
        return Status::Unavailable("allocation identity space is exhausted");
    LockExtents();
    const auto data_extent = AllocateExtentLocked(&header_->data_free_head, data_extent_bytes,
                                                   alignof(AllocationDescriptor));
    if (!data_extent.ok()) {
        UnlockExtents();
        return data_extent.status();
    }
    const auto coherence_extent = AllocateExtentLocked(&header_->coherence_free_head, coherence_extent_bytes,
                                                        alignof(CoherenceBlockDescriptor));
    if (!coherence_extent.ok()) {
        FreeExtentLocked(&header_->data_free_head, data_extent.value(), data_extent_bytes);
        UnlockExtents();
        return coherence_extent.status();
    }
    UnlockExtents();

    const auto object_offset = AlignUp(data_extent.value() + sizeof(AllocationDescriptor), alignment);
    auto* descriptor = reinterpret_cast<AllocationDescriptor*>(region_base_ + object_offset -
                                                                sizeof(AllocationDescriptor));
    new (descriptor) AllocationDescriptor();
    descriptor->magic = kAllocationMagic;
    descriptor->owner_host = local_host_;
    descriptor->allocation_id = allocation_id;
    descriptor->object_offset = object_offset;
    descriptor->bytes = bytes;
    descriptor->alignment = alignment;
    descriptor->coherence_block_bytes = block_bytes;
    descriptor->coherence_block_count = block_count;
    descriptor->coherence_metadata_offset = coherence_extent.value();
    descriptor->data_extent_offset = data_extent.value();
    descriptor->data_extent_bytes = data_extent_bytes;
    descriptor->coherence_extent_bytes = coherence_extent_bytes;
    descriptor->object_version.store(0, std::memory_order_relaxed);
    descriptor->range_commit_epoch.store(0, std::memory_order_relaxed);
    auto* blocks = reinterpret_cast<CoherenceBlockDescriptor*>(region_base_ + coherence_extent.value());
    for (std::uint64_t index = 0; index < block_count; ++index) {
        new (&blocks[index]) CoherenceBlockDescriptor();
        blocks[index].token_owner.store(local_host_, std::memory_order_relaxed);
        blocks[index].version.store(0, std::memory_order_relaxed);
        blocks[index].token_epoch.store(1, std::memory_order_relaxed);
        blocks[index].writeback_epoch.store(0, std::memory_order_relaxed);
    }
    descriptor->state.store(static_cast<std::uint32_t>(AllocationState::kAllocated), std::memory_order_release);
    return GlobalPointer {0, object_offset};
}

Result<AllocationInfo> SharedExtentAllocator::Describe(GlobalPointer gptr) const {
    if (!initialized_)
        return Status::FailedPrecondition("shared allocator is not initialized");
    const auto pool_end = expected_data_offset_ + expected_data_bytes_;
    if (gptr.region_id != 0 || gptr.offset < expected_data_offset_ + sizeof(AllocationDescriptor) ||
        gptr.offset >= pool_end) {
        return Status::InvalidArgument("global pointer is outside the shared allocation pool");
    }
    const auto descriptor_offset = gptr.offset - sizeof(AllocationDescriptor);
    if (descriptor_offset % alignof(AllocationDescriptor) != 0) {
        return Status::NotFound("global pointer is not an allocation base");
    }
    const auto* descriptor = reinterpret_cast<const AllocationDescriptor*>(region_base_ + descriptor_offset);
    const auto state = static_cast<AllocationState>(descriptor->state.load(std::memory_order_acquire));
    if (state != AllocationState::kAllocated || descriptor->magic != kAllocationMagic ||
        descriptor->object_offset != gptr.offset || descriptor->bytes == 0 ||
        descriptor->bytes > pool_end - descriptor->object_offset || descriptor->owner_host >= kMaxHosts) {
        return Status::NotFound("global pointer is not a published allocation base");
    }
    return AllocationInfo {gptr,
                           descriptor->bytes,
                           descriptor->alignment,
                           descriptor->owner_host,
                           descriptor->allocation_id,
                           descriptor->coherence_block_bytes,
                           descriptor->coherence_block_count,
                           descriptor->coherence_metadata_offset};
}

Result<AllocationDescriptor*> SharedExtentAllocator::MutableDescriptor(GlobalPointer gptr, bool allow_retiring) const {
    const auto allocation = Describe(gptr);
    if (!allocation.ok()) {
        if (!allow_retiring || gptr.region_id != 0 ||
            gptr.offset < expected_data_offset_ + sizeof(AllocationDescriptor) ||
            gptr.offset >= expected_data_offset_ + expected_data_bytes_)
            return allocation.status();
        auto* descriptor = reinterpret_cast<AllocationDescriptor*>(region_base_ + gptr.offset -
                                                                    sizeof(AllocationDescriptor));
        if (descriptor->magic != kAllocationMagic || descriptor->object_offset != gptr.offset ||
            descriptor->state.load(std::memory_order_acquire) !=
                static_cast<std::uint32_t>(AllocationState::kRetiring))
            return allocation.status();
        return descriptor;
    }
    return reinterpret_cast<AllocationDescriptor*>(region_base_ + gptr.offset - sizeof(AllocationDescriptor));
}

Result<AllocationDescriptor*> SharedExtentAllocator::AcquireReference(GlobalPointer gptr, HostId host) const {
    if (host >= kMaxHosts)
        return Status::InvalidArgument("reference host is out of range");
    const auto descriptor_result = MutableDescriptor(gptr);
    if (!descriptor_result.ok())
        return descriptor_result.status();
    auto* descriptor = descriptor_result.value();
    const auto allocation_id = descriptor->allocation_id;
    descriptor->active_references[host].fetch_add(1, std::memory_order_acq_rel);
    if (descriptor->state.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(AllocationState::kAllocated) ||
        descriptor->allocation_id != allocation_id) {
        descriptor->active_references[host].fetch_sub(1, std::memory_order_release);
        return Status::FailedPrecondition("allocation began retiring while acquiring a reference");
    }
    return descriptor;
}

Status SharedExtentAllocator::ReleaseReference(AllocationDescriptor* descriptor, std::uint64_t allocation_id,
                                             HostId host) const {
    if (descriptor == nullptr || host >= kMaxHosts)
        return Status::InvalidArgument("invalid object reference release");
    // The descriptor remains allocated while any reference is non-zero, so
    // allocation_id is stable until this decrement completes.
    if (descriptor->allocation_id != allocation_id)
        return Status::FailedPrecondition("object reference belongs to another allocation");
    const auto previous = descriptor->active_references[host].fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
        descriptor->active_references[host].fetch_add(1, std::memory_order_relaxed);
        return Status::FailedPrecondition("object reference count underflow");
    }
    return Status::Ok();
}

Status SharedExtentAllocator::CancelRetire(AllocationDescriptor* descriptor, std::uint64_t allocation_id) const {
    if (descriptor == nullptr || descriptor->allocation_id != allocation_id)
        return Status::FailedPrecondition("retirement rollback targets another allocation");
    auto expected = static_cast<std::uint32_t>(AllocationState::kRetiring);
    if (!descriptor->state.compare_exchange_strong(expected,
                                                   static_cast<std::uint32_t>(AllocationState::kAllocated),
                                                   std::memory_order_release, std::memory_order_acquire))
        return Status::FailedPrecondition("allocation is not retiring");
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

Result<HostId> SharedExtentAllocator::OwningHost(GlobalPointer gptr) const {
    const auto allocation = Describe(gptr);
    if (!allocation.ok())
        return allocation.status();
    return allocation.value().owner_host;
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

Result<std::uint32_t> SharedExtentAllocator::ReserveExtentNodeLocked() const {
    for (std::uint32_t index = 0; index < header_->extent_node_capacity; ++index) {
        if (header_->extent_nodes[index].state == static_cast<std::uint32_t>(SharedExtentState::kUnused))
            return index;
    }
    return Status::Unavailable("shared extent metadata is exhausted");
}

Result<std::uint64_t> SharedExtentAllocator::AllocateExtentLocked(std::uint32_t* head, std::uint64_t bytes,
                                                                std::size_t alignment) const {
    std::uint32_t previous = kInvalidExtentIndex;
    for (auto index = *head; index != kInvalidExtentIndex; index = header_->extent_nodes[index].next) {
        auto& node = header_->extent_nodes[index];
        const auto allocated_offset = AlignUp(node.offset, alignment);
        if (allocated_offset < node.offset || allocated_offset > node.offset + node.bytes ||
            bytes > node.offset + node.bytes - allocated_offset) {
            previous = index;
            continue;
        }
        const auto prefix = allocated_offset - node.offset;
        const auto suffix_offset = allocated_offset + bytes;
        const auto suffix = node.offset + node.bytes - suffix_offset;
        std::uint32_t suffix_node = kInvalidExtentIndex;
        if (prefix != 0 && suffix != 0) {
            const auto reserved = ReserveExtentNodeLocked();
            if (!reserved.ok())
                return reserved.status();
            suffix_node = reserved.value();
        }
        if (prefix != 0) {
            node.bytes = prefix;
            if (suffix != 0) {
                auto& extra = header_->extent_nodes[suffix_node];
                extra = {suffix_offset, suffix, node.next,
                         static_cast<std::uint32_t>(SharedExtentState::kFree)};
                node.next = suffix_node;
            }
        } else if (suffix != 0) {
            node.offset = suffix_offset;
            node.bytes = suffix;
        } else {
            const auto next = node.next;
            node = {};
            node.next = kInvalidExtentIndex;
            if (previous == kInvalidExtentIndex)
                *head = next;
            else
                header_->extent_nodes[previous].next = next;
        }
        return allocated_offset;
    }
    return Status::Unavailable("shared extent pool is exhausted");
}

Status SharedExtentAllocator::FreeExtentLocked(std::uint32_t* head, std::uint64_t offset,
                                             std::uint64_t bytes) const {
    if (bytes == 0)
        return Status::InvalidArgument("cannot free an empty extent");
    std::uint32_t previous = kInvalidExtentIndex;
    auto next = *head;
    while (next != kInvalidExtentIndex && header_->extent_nodes[next].offset < offset) {
        previous = next;
        next = header_->extent_nodes[next].next;
    }
    if ((previous != kInvalidExtentIndex &&
         header_->extent_nodes[previous].offset + header_->extent_nodes[previous].bytes > offset) ||
        (next != kInvalidExtentIndex && offset + bytes > header_->extent_nodes[next].offset))
        return Status::FailedPrecondition("freed extent overlaps the free pool");
    const auto reserved = ReserveExtentNodeLocked();
    if (!reserved.ok())
        return reserved.status();
    const auto index = reserved.value();
    header_->extent_nodes[index] = {offset, bytes, next, static_cast<std::uint32_t>(SharedExtentState::kFree)};
    if (previous == kInvalidExtentIndex)
        *head = index;
    else
        header_->extent_nodes[previous].next = index;

    auto merged = index;
    if (previous != kInvalidExtentIndex &&
        header_->extent_nodes[previous].offset + header_->extent_nodes[previous].bytes == offset) {
        header_->extent_nodes[previous].bytes += bytes;
        header_->extent_nodes[previous].next = next;
        header_->extent_nodes[index] = {};
        header_->extent_nodes[index].next = kInvalidExtentIndex;
        merged = previous;
    }
    next = header_->extent_nodes[merged].next;
    if (next != kInvalidExtentIndex &&
        header_->extent_nodes[merged].offset + header_->extent_nodes[merged].bytes ==
            header_->extent_nodes[next].offset) {
        header_->extent_nodes[merged].bytes += header_->extent_nodes[next].bytes;
        header_->extent_nodes[merged].next = header_->extent_nodes[next].next;
        header_->extent_nodes[next] = {};
        header_->extent_nodes[next].next = kInvalidExtentIndex;
    }
    return Status::Ok();
}

Status SharedExtentAllocator::Free(GlobalPointer gptr) {
    const auto pool_end = expected_data_offset_ + expected_data_bytes_;
    if (!initialized_ || gptr.region_id != 0 ||
        gptr.offset < expected_data_offset_ + sizeof(AllocationDescriptor) || gptr.offset >= pool_end)
        return Status::InvalidArgument("global pointer is outside the shared allocation pool");
    auto* descriptor = reinterpret_cast<AllocationDescriptor*>(region_base_ + gptr.offset -
                                                                sizeof(AllocationDescriptor));
    if (descriptor->magic != kAllocationMagic || descriptor->object_offset != gptr.offset)
        return Status::NotFound("global pointer is not an allocation base");
    if (descriptor->owner_host != local_host_)
        return Status::FailedPrecondition("only the allocation owner may reclaim shared storage");
    auto state = descriptor->state.load(std::memory_order_acquire);
    if (state == static_cast<std::uint32_t>(AllocationState::kAllocated)) {
        if (!descriptor->state.compare_exchange_strong(state, static_cast<std::uint32_t>(AllocationState::kRetiring),
                                                       std::memory_order_acq_rel, std::memory_order_acquire))
            return Status::NotFound("allocation state changed during retirement");
    } else if (state != static_cast<std::uint32_t>(AllocationState::kRetiring)) {
        return Status::NotFound("allocation is already free");
    }
    const auto data_offset = descriptor->data_extent_offset;
    const auto data_bytes = descriptor->data_extent_bytes;
    const auto coherence_offset = descriptor->coherence_metadata_offset;
    const auto coherence_bytes = descriptor->coherence_extent_bytes;
    LockExtents();
    std::size_t available_nodes = 0;
    for (std::uint32_t index = 0; index < header_->extent_node_capacity; ++index)
        available_nodes += header_->extent_nodes[index].state ==
                           static_cast<std::uint32_t>(SharedExtentState::kUnused);
    if (available_nodes < 2) {
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
    }
    UnlockExtents();
    return !data_status.ok() ? data_status : coherence_status;
}

}  // namespace cxloom::loommem
