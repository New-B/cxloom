#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cxloom/common/status.h"
#include "cxloom/common/types.h"

namespace cxloom::loommem {

class GlobalAllocator {
public:
    virtual ~GlobalAllocator() = default;

    virtual Status Initialize() = 0;
    virtual Result<GlobalPointer> Allocate(std::size_t bytes, std::size_t alignment) = 0;
    virtual Status Free(GlobalPointer gptr) = 0;
};

// Uses fixed-size slabs for small objects and coalescing extents for large ones.
// The metadata is process-local in this prototype; it will move into the CXL
// allocator region when multi-host bootstrap metadata is introduced.
class SlabExtentAllocator final : public GlobalAllocator {
public:
    explicit SlabExtentAllocator(std::size_t shared_region_bytes);

    Status Initialize() override;
    Result<GlobalPointer> Allocate(std::size_t bytes, std::size_t alignment) override;
    Status Free(GlobalPointer gptr) override;

private:
    enum class AllocationKind : std::uint8_t {
        kSlab,
        kExtent,
    };

    struct AllocationRecord {
        AllocationKind kind {AllocationKind::kExtent};
        std::uint64_t span_bytes {0};
    };

    static constexpr std::uint64_t kSlabPageBytes = 64ULL << 10;
    static constexpr std::uint64_t kMaxSlabObjectBytes = kSlabPageBytes;

    Result<GlobalPointer> AllocateSlab(std::size_t bytes, std::size_t alignment);
    Result<GlobalPointer> AllocateExtent(std::size_t bytes, std::size_t alignment);
    Status ReserveSlabPage(std::uint64_t object_bytes);
    Result<std::uint64_t> ReserveExtent(std::uint64_t bytes, std::size_t alignment);
    void InsertFreeExtent(std::uint64_t offset, std::uint64_t bytes);

    std::size_t shared_region_bytes_ {0};
    std::map<std::uint64_t, std::uint64_t> free_extents_;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> slab_free_blocks_;
    std::unordered_map<std::uint64_t, AllocationRecord> allocations_;
    mutable std::mutex mutex_;
    bool initialized_ {false};
};

}  // namespace cxloom::loommem
