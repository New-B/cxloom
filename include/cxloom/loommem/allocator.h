#pragma once

#include <array>
#include <atomic>
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

// Uses fixed-size slabs for small objects and coalescing extents for large
// ones. The metadata is process-local in this prototype; it will move into the
// CXL allocator region when multi-host bootstrap metadata is introduced.
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

inline constexpr std::uint64_t kAllocatorMagic = 0x43584c4f4f4d414cULL;
inline constexpr std::uint64_t kAllocationMagic = 0x43584c4f4f4d4f42ULL;
inline constexpr std::uint32_t kAllocatorLayoutVersion = 3;

enum class AllocatorState : std::uint32_t {
    kUninitialized = 0,
    kInitializing = 1,
    kReady = 2,
    kFailed = 3,
};

enum class AllocationState : std::uint32_t {
    kEmpty = 0,
    kAllocated = 1,
};

struct AllocationInfo {
    GlobalPointer gptr {};
    std::uint64_t bytes {0};
    std::uint64_t alignment {0};
    HostId owner_host {0};
    std::uint64_t allocation_id {0};
    std::uint64_t generation {0};
};

struct alignas(64) AllocationDescriptor {
    std::uint64_t magic {0};
    std::atomic<std::uint32_t> state {static_cast<std::uint32_t>(AllocationState::kEmpty)};
    HostId owner_host {0};
    std::uint16_t reserved0 {0};
    std::uint64_t allocation_id {0};
    std::uint64_t object_offset {0};
    std::uint64_t bytes {0};
    std::uint64_t alignment {0};
    std::uint64_t generation {0};
};

struct alignas(64) AllocatorHeader {
    std::uint64_t magic {0};
    std::uint32_t layout_version {0};
    std::uint32_t header_bytes {0};
    std::atomic<std::uint32_t> state {static_cast<std::uint32_t>(AllocatorState::kUninitialized)};
    std::uint32_t reserved0 {0};
    std::uint64_t shared_data_offset {0};
    std::uint64_t shared_data_bytes {0};
    std::atomic<std::uint64_t> next_offset {0};
    std::atomic<std::uint64_t> allocation_count {0};
};

Status FormatSharedAllocator(AllocatorHeader* header, std::size_t allocator_region_bytes,
                             std::uint64_t shared_data_offset, std::uint64_t shared_data_bytes);

class SharedBumpAllocator final : public GlobalAllocator {
  public:
    SharedBumpAllocator(AllocatorHeader* header, void* region_base, std::size_t region_bytes, HostId local_host,
                        std::uint64_t expected_data_offset, std::uint64_t expected_data_bytes);

    Status Initialize() override;
    Result<GlobalPointer> Allocate(std::size_t bytes, std::size_t alignment) override;
    Status Free(GlobalPointer gptr) override;
    Result<AllocationInfo> Describe(GlobalPointer gptr) const;
    Result<HostId> OwningHost(GlobalPointer gptr) const;

  private:
    AllocatorHeader* header_ {nullptr};
    std::byte* region_base_ {nullptr};
    std::size_t region_bytes_ {0};
    HostId local_host_ {0};
    std::uint64_t expected_data_offset_ {0};
    std::uint64_t expected_data_bytes_ {0};
    bool initialized_ {false};
};

}  // namespace cxloom::loommem
