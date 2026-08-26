#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cxloom {

using HostId = std::uint16_t;
using QueueId = std::uint32_t;
using ThreadLocalId = std::uint64_t;
using Version = std::uint64_t;
using ObjectId = std::uint64_t;

struct GlobalPointer {
    std::uint32_t region_id {0};
    std::uint64_t offset {0};
};

struct GlobalThreadId {
    HostId home_host {0};
    ThreadLocalId local_tid {0};
};

struct ThreadPlacementHint {
    bool has_explicit_host {false};
    HostId explicit_host {0};
    bool has_dominant_gptr {false};
    GlobalPointer dominant_gptr {};
    std::string label;
};

struct HostLoadSnapshot {
    HostId host_id {0};
    std::uint32_t running_threads {0};
    std::uint32_t pending_creates {0};
};

inline bool operator==(const GlobalPointer& lhs, const GlobalPointer& rhs) {
    return lhs.region_id == rhs.region_id && lhs.offset == rhs.offset;
}

inline bool operator==(const GlobalThreadId& lhs, const GlobalThreadId& rhs) {
    return lhs.home_host == rhs.home_host && lhs.local_tid == rhs.local_tid;
}

}  // namespace cxloom

