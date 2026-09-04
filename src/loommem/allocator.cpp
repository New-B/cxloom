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
                             std::uint64_t shared_data_offset, std::uint64_t shared_data_bytes) {
    if (header == nullptr || allocator_region_bytes < sizeof(AllocatorHeader)) {
        return Status::InvalidArgument("allocator region is too small for its shared header");
    }
    if (shared_data_bytes <= sizeof(AllocationDescriptor)) {
        return Status::InvalidArgument("shared data region is too small for allocations");
    }
    header = new (header) AllocatorHeader();
    header->state.store(static_cast<std::uint32_t>(AllocatorState::kInitializing), std::memory_order_relaxed);
    header->magic = kAllocatorMagic;
    header->layout_version = kAllocatorLayoutVersion;
    header->header_bytes = sizeof(AllocatorHeader);
    header->shared_data_offset = shared_data_offset;
    header->shared_data_bytes = shared_data_bytes;
    header->next_offset.store(shared_data_offset, std::memory_order_relaxed);
    header->allocation_count.store(0, std::memory_order_relaxed);
    header->state.store(static_cast<std::uint32_t>(AllocatorState::kReady), std::memory_order_release);
    return Status::Ok();
}

SharedBumpAllocator::SharedBumpAllocator(AllocatorHeader* header, void* region_base, std::size_t region_bytes,
                                         HostId local_host, std::uint64_t expected_data_offset,
                                         std::uint64_t expected_data_bytes)
    : header_(header), region_base_(static_cast<std::byte*>(region_base)), region_bytes_(region_bytes),
      local_host_(local_host), expected_data_offset_(expected_data_offset), expected_data_bytes_(expected_data_bytes) {}

Status SharedBumpAllocator::Initialize() {
    if (header_ == nullptr || region_base_ == nullptr || header_->magic != kAllocatorMagic ||
        header_->layout_version != kAllocatorLayoutVersion || header_->header_bytes != sizeof(AllocatorHeader)) {
        return Status::FailedPrecondition("shared allocator header is incompatible");
    }
    if (static_cast<AllocatorState>(header_->state.load(std::memory_order_acquire)) != AllocatorState::kReady) {
        return Status::FailedPrecondition("shared allocator is not ready");
    }
    if (!header_->state.is_lock_free() || !header_->next_offset.is_lock_free() ||
        !header_->allocation_count.is_lock_free()) {
        return Status::FailedPrecondition("shared allocator requires lock-free shared atomics");
    }
    if (header_->shared_data_offset != expected_data_offset_ || header_->shared_data_bytes != expected_data_bytes_ ||
        expected_data_offset_ > region_bytes_ || expected_data_bytes_ > region_bytes_ - expected_data_offset_) {
        return Status::FailedPrecondition("shared allocator layout does not match runtime configuration");
    }
    initialized_ = true;
    return Status::Ok();
}

Result<GlobalPointer> SharedBumpAllocator::Allocate(std::size_t bytes, std::size_t alignment) {
    if (!initialized_)
        return Status::FailedPrecondition("shared allocator is not initialized");
    if (bytes == 0)
        return Status::InvalidArgument("allocation size must be non-zero");
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return Status::InvalidArgument("alignment must be a non-zero power of two");
    }

    const auto effective_alignment = std::max<std::size_t>(alignment, alignof(AllocationDescriptor));
    const auto pool_end = expected_data_offset_ + expected_data_bytes_;
    std::uint64_t current = header_->next_offset.load(std::memory_order_relaxed);
    std::uint64_t object_offset = 0;
    while (true) {
        if (current > pool_end || sizeof(AllocationDescriptor) > pool_end - current) {
            return Status::Unavailable("shared allocation pool is exhausted");
        }
        object_offset = AlignUp(current + sizeof(AllocationDescriptor), effective_alignment);
        if (object_offset < current || object_offset > pool_end || bytes > pool_end - object_offset) {
            return Status::Unavailable("shared allocation pool is exhausted");
        }
        const auto allocation_end = object_offset + bytes;
        if (header_->next_offset.compare_exchange_weak(current, allocation_end, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
            break;
        }
    }

    const auto allocation_id = header_->allocation_count.fetch_add(1, std::memory_order_relaxed) + 1;
    auto* descriptor =
        reinterpret_cast<AllocationDescriptor*>(region_base_ + object_offset - sizeof(AllocationDescriptor));
    new (descriptor) AllocationDescriptor();
    descriptor->magic = kAllocationMagic;
    descriptor->owner_host = local_host_;
    descriptor->allocation_id = allocation_id;
    descriptor->object_offset = object_offset;
    descriptor->bytes = bytes;
    descriptor->alignment = alignment;
    descriptor->generation = 1;
    descriptor->state.store(static_cast<std::uint32_t>(AllocationState::kAllocated), std::memory_order_release);
    return GlobalPointer {0, object_offset};
}

Result<AllocationInfo> SharedBumpAllocator::Describe(GlobalPointer gptr) const {
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
                           descriptor->generation};
}

Result<HostId> SharedBumpAllocator::OwningHost(GlobalPointer gptr) const {
    const auto allocation = Describe(gptr);
    if (!allocation.ok())
        return allocation.status();
    return allocation.value().owner_host;
}

Status SharedBumpAllocator::Free(GlobalPointer) {
    return Status::Unimplemented("shared bump allocations cannot be freed yet");
}

}  // namespace cxloom::loommem
