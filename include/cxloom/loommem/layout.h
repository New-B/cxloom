#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

#include "cxloom/common/types.h"

namespace cxloom::loommem {

enum class RegionKind : std::uint32_t {
    kBootstrap = 0,
    kAllocator = 1,
    kCoherence = 2,
    kQueues = 3,
    kSharedData = 4,
};

struct RegionRange {
    RegionKind kind {RegionKind::kBootstrap};
    std::uint64_t offset {0};
    std::uint64_t bytes {0};
};

struct SharedRegionLayout {
    RegionRange bootstrap {};
    RegionRange allocator {};
    RegionRange coherence {};
    RegionRange queues {};
    RegionRange shared_data {};
};

inline constexpr std::uint64_t kBootstrapMagic = 0x43584c4f4f4d424dULL;  // "CXLOOMBM"
inline constexpr std::uint32_t kBootstrapLayoutVersion = 1;

enum class BootstrapState : std::uint32_t {
    kUninitialized = 0,
    kInitializing = 1,
    kReady = 2,
    kFailed = 3,
};

// Resides at offset zero of every shared CXL region. Fields are intentionally
// pointer-free so every host can interpret the same bytes at a different VA.
struct alignas(64) BootstrapHeader {
    std::uint64_t magic {0};
    std::uint32_t layout_version {0};
    std::uint32_t header_bytes {0};
    std::atomic<std::uint32_t> state {static_cast<std::uint32_t>(BootstrapState::kUninitialized)};
    std::uint32_t reserved0 {0};
    std::uint64_t region_bytes {0};
    std::uint16_t host_count {0};
    std::uint16_t reserved1 {0};
    std::uint32_t reserved2 {0};
    SharedRegionLayout layout {};
};

struct ObjectMetadata {
    ObjectId object_id {0};
    GlobalPointer base {};
    std::size_t bytes {0};
    Version global_version {0};
    HostId token_owner {0};
    std::uint32_t flags {0};
};

struct ReplicaMetadata {
    ObjectId object_id {0};
    void* local_addr {nullptr};
    Version local_version {0};
    bool cached {false};
    bool dirty {false};
};

SharedRegionLayout BuildDefaultLayout(std::size_t total_bytes);

}  // namespace cxloom::loommem
