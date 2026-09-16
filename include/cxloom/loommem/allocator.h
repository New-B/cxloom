#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cxloom/common/status.h"
#include "cxloom/common/config.h"
#include "cxloom/common/types.h"

namespace cxloom::loommem {

class GlobalAllocator {
  public:
    virtual ~GlobalAllocator() = default;

    virtual Status Initialize() = 0;
    virtual Result<GlobalPointer> Allocate(std::size_t bytes, std::size_t alignment) = 0;
    virtual Status Free(GlobalPointer gptr) = 0;
};

inline constexpr std::uint64_t kAllocatorMagic = 0x43584c4f4f4d414cULL;
inline constexpr std::uint64_t kAllocationMagic = 0x43584c4f4f4d4f42ULL;
inline constexpr std::uint32_t kAllocatorLayoutVersion = 11;
inline constexpr std::uint32_t kInvalidExtentIndex = UINT32_MAX;
// Bounded shared metadata keeps the allocator header within the 256 KiB
// bootstrap mapping while still allowing thousands of extents.
inline constexpr std::size_t kMaxSharedExtentNodes = 4096;
inline constexpr std::size_t kTlsfBinCount = 64;

enum class AllocatorState : std::uint32_t {
    kUninitialized = 0,
    kInitializing = 1,
    kReady = 2,
    kFailed = 3,
};

enum class AllocationState : std::uint32_t {
    kEmpty = 0,
    kAllocated = 1,
    kRetiring = 2,
};

struct AllocationInfo {
    GlobalPointer gptr {};
    std::uint64_t bytes {0};
    std::uint64_t alignment {0};
    HostId owner_host {0};
    std::uint64_t coherence_block_bytes {0};
    std::uint64_t coherence_block_count {0};
    std::uint64_t coherence_metadata_offset {0};
};

struct AllocationOptions {
    std::size_t bytes {0};
    std::size_t alignment {0};
    // Zero uses the allocator default block size.
    std::size_t coherence_block_bytes {0};
};

inline constexpr std::uint64_t kCoherenceRegionMagic = 0x43584c4f4f4d4348ULL;
inline constexpr std::uint32_t kCoherenceRegionLayoutVersion = 2;

struct alignas(64) CoherenceBlockDescriptor {
    std::atomic<std::uint32_t> token_owner {0};
    std::atomic<std::uint64_t> version {0};
    std::atomic<std::uint64_t> token_epoch {0};
    std::atomic<std::uint64_t> writeback_epoch {0};
};

struct alignas(64) CoherenceRegionHeader {
    std::uint64_t magic {0};
    std::uint32_t layout_version {0};
    std::uint32_t header_bytes {0};
    std::uint64_t region_offset {0};
    std::uint64_t region_bytes {0};
};

Status FormatCoherenceRegion(CoherenceRegionHeader* header, std::uint64_t region_offset,
                             std::uint64_t region_bytes);

struct alignas(64) AllocationDescriptor {
    std::uint64_t magic {0};
    std::atomic<std::uint32_t> state {static_cast<std::uint32_t>(AllocationState::kEmpty)};
    HostId owner_host {0};
    std::uint16_t reserved0 {0};
    std::uint64_t object_offset {0};
    std::uint64_t bytes {0};
    std::uint64_t alignment {0};
    std::uint64_t coherence_block_bytes {0};
    std::uint64_t coherence_block_count {0};
    std::uint64_t coherence_metadata_offset {0};
    std::uint64_t data_extent_offset {0};
    std::uint64_t data_extent_bytes {0};
    std::uint64_t coherence_extent_bytes {0};
    std::array<std::atomic<std::uint64_t>, kMaxHosts> active_references {};
};

enum class SharedExtentState : std::uint32_t { kUnused = 0, kFree = 1 };

struct SharedExtentNode {
    std::uint64_t offset {0};
    std::uint64_t bytes {0};
    // Address-ordered list (used for coalescing).
    std::uint32_t next {kInvalidExtentIndex};
    std::uint32_t previous {kInvalidExtentIndex};
    // Intrusive size-bin list (used for allocation lookup).
    std::uint32_t bin_next {kInvalidExtentIndex};
    std::uint32_t bin_previous {kInvalidExtentIndex};
    std::uint16_t bin {0};
    std::uint16_t reserved {0};
    std::uint32_t state {static_cast<std::uint32_t>(SharedExtentState::kUnused)};
};

enum class RetirementPhase : std::uint32_t { kIdle, kClosing, kDraining, kCleaning, kReclaimable };

struct RetirementSnapshot {
    RetirementPhase phase {RetirementPhase::kIdle};
    GlobalPointer object {};
    std::uint64_t sequence {0};
    HostId coordinator {0};
    std::uint16_t host_count {0};
};

// One transaction at a time. Protected by extent_lock; watermarks are published
// with the corresponding host acknowledgement. No per-allocation generation.
struct RetirementControl {
    RetirementSnapshot current {};
    std::uint64_t closed {0};
    std::uint64_t drained {0};
    std::uint64_t cleaned {0};
    std::array<std::array<std::uint64_t, kMaxHosts>, kMaxHosts> watermarks {};
};

struct alignas(64) AllocatorHeader {
    std::uint64_t magic {0};
    std::uint32_t layout_version {0};
    std::uint32_t header_bytes {0};
    std::atomic<std::uint32_t> state {static_cast<std::uint32_t>(AllocatorState::kUninitialized)};
    std::uint32_t reserved0 {0};
    std::uint64_t shared_data_offset {0};
    std::uint64_t shared_data_bytes {0};
    std::atomic<std::uint32_t> extent_lock {0};
    std::uint32_t data_free_head {kInvalidExtentIndex};
    std::uint32_t coherence_free_head {kInvalidExtentIndex};
    std::array<std::uint32_t, kTlsfBinCount> data_bins {};
    std::array<std::uint32_t, kTlsfBinCount> coherence_bins {};
    std::uint64_t data_bin_bitmap {0};
    std::uint64_t coherence_bin_bitmap {0};
    std::uint32_t free_extent_node_head {kInvalidExtentIndex};
    std::uint32_t extent_node_capacity {0};
    std::uint32_t reserved1 {0};
    std::array<SharedExtentNode, kMaxSharedExtentNodes> extent_nodes {};
    RetirementControl retirement {};
};

Status FormatSharedAllocator(AllocatorHeader* header, std::size_t allocator_region_bytes,
                             std::uint64_t shared_data_offset, std::uint64_t shared_data_bytes,
                             std::uint64_t coherence_offset, std::uint64_t coherence_bytes);

class SharedExtentAllocator final : public GlobalAllocator {
  public:
    SharedExtentAllocator(AllocatorHeader* header, void* region_base, std::size_t region_bytes, HostId local_host,
                        std::uint64_t expected_data_offset, std::uint64_t expected_data_bytes,
                        CoherenceRegionHeader* coherence_header = nullptr,
                        std::size_t default_block_bytes = 4096);

    Status Initialize() override;
    Result<GlobalPointer> Allocate(std::size_t bytes, std::size_t alignment) override;
    // Reclaim primitive: requires the matching all-host completion certificate.
    Status Free(GlobalPointer gptr) override;
    Result<AllocationInfo> Describe(GlobalPointer gptr) const;
    Result<HostId> OwningHost(GlobalPointer gptr) const;
    // Internal lookup; callers must hold a reference or own the retirement phase
    // while dereferencing the result. Describe returns a locked metadata copy.
    Result<AllocationDescriptor*> MutableDescriptor(GlobalPointer gptr, bool allow_retiring = false) const;
    Result<AllocationDescriptor*> AcquireReference(GlobalPointer gptr, HostId host, bool allow_retiring = false) const;
    Status ReleaseReference(AllocationDescriptor* descriptor, HostId host) const;
    Result<RetirementSnapshot> BeginRetire(GlobalPointer object, std::uint16_t host_count);
    RetirementSnapshot Retirement() const;
    bool IsRetiring(GlobalPointer object) const;
    Status AcknowledgeRetirement(const RetirementSnapshot& transaction, HostId host,
                                const std::array<std::uint64_t, kMaxHosts>& cursors);
    bool RetirementAcknowledged(const RetirementSnapshot& transaction, HostId host) const;
    Result<CoherenceBlockDescriptor*> MutableCoherenceBlock(GlobalPointer gptr, std::uint64_t block_index,
                                                             bool allow_retiring = false) const;
    Result<GlobalPointer> Allocate(const AllocationOptions& options);

  private:
    AllocatorHeader* header_ {nullptr};
    std::byte* region_base_ {nullptr};
    std::size_t region_bytes_ {0};
    HostId local_host_ {0};
    std::uint64_t expected_data_offset_ {0};
    std::uint64_t expected_data_bytes_ {0};
    CoherenceRegionHeader* coherence_header_ {nullptr};
    std::size_t default_block_bytes_ {4096};
    bool initialized_ {false};

    Result<AllocationDescriptor*> FindDescriptorLocked(GlobalPointer object, bool allow_retiring) const;
    void LockExtents() const;
    void UnlockExtents() const;
    Result<std::uint32_t> ReserveExtentNodeLocked() const;
    Result<std::uint64_t> AllocateExtentLocked(std::uint32_t* head, std::uint64_t bytes,
                                               std::size_t alignment) const;
    Status FreeExtentLocked(std::uint32_t* head, std::uint64_t offset, std::uint64_t bytes) const;
    std::uint16_t BinForBytes(std::uint64_t bytes) const;
    void InsertBinLocked(std::uint32_t* head, std::uint64_t* bitmap, std::uint32_t index) const;
    void RemoveBinLocked(std::uint32_t* head, std::uint64_t* bitmap, std::uint32_t index) const;
};

}  // namespace cxloom::loommem
