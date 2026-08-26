#pragma once

#include <cstddef>
#include <cstdint>

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

