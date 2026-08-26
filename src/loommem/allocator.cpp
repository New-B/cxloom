#include "cxloom/loommem/allocator.h"

#include <algorithm>
#include <limits>

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

SlabExtentAllocator::SlabExtentAllocator(std::size_t shared_region_bytes)
    : shared_region_bytes_(shared_region_bytes) {}

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
    allocations_[offset] = AllocationRecord{AllocationKind::kSlab, object_bytes};
    return GlobalPointer{0, offset};
}

Result<GlobalPointer> SlabExtentAllocator::AllocateExtent(std::size_t bytes, std::size_t alignment) {
    const auto offset = ReserveExtent(bytes, alignment);
    if (!offset.ok()) {
        return offset.status();
    }
    allocations_[offset.value()] = AllocationRecord{AllocationKind::kExtent, bytes};
    return GlobalPointer{0, offset.value()};
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

}  // namespace cxloom::loommem
