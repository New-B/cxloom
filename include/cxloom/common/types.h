#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cxloom {

using HostId = std::uint16_t;
using QueueId = std::uint32_t;
using ThreadLocalId = std::uint64_t;
using Version = std::uint64_t;
using ObjectId = std::uint64_t;

inline constexpr std::size_t kMaxHosts = 64;

struct GlobalPointer {
    std::uint32_t region_id {0};
    std::uint64_t offset {0};
};

struct GlobalThreadId {
    HostId home_host {0};
    ThreadLocalId local_tid {0};
};

enum class MemoryAccess { kRead, kWrite, kReadWrite };
struct WorkingSetEntry {
    GlobalPointer object;
    std::uint64_t offset {0}, bytes {0}; // bytes=0 means remainder of allocation
    MemoryAccess access {MemoryAccess::kRead};
    double weight {1.0};
};
struct BlockLocality {
    std::uint64_t bytes, version;
    HostId token_owner, last_writer; // kMaxHosts means no published writer yet
    std::uint64_t current_replica_hosts;
};

struct ThreadPlacementHint {
    bool has_explicit_host {false};
    HostId explicit_host {0};
    bool has_dominant_gptr {false};
    GlobalPointer dominant_gptr {};
    std::string label;
    std::vector<WorkingSetEntry> working_set;
};

struct ExecutionLoad {
    std::uint32_t executing {0}, ready {0}, blocked {0}, workers {0};
};

struct HostLoadSnapshot {
    HostId host_id {0};
    std::uint32_t running_threads {0};
    std::uint32_t pending_creates {0};
    std::uint32_t queued_messages {0};
    std::uint64_t sample_sequence {0};
    std::uint64_t sampled_at_ns {0};
    ExecutionLoad execution {}; // workers=0: legacy count-only sample

};

inline bool operator==(const GlobalPointer& lhs, const GlobalPointer& rhs) {
    return lhs.region_id == rhs.region_id && lhs.offset == rhs.offset;
}

inline bool operator==(const GlobalThreadId& lhs, const GlobalThreadId& rhs) {
    return lhs.home_host == rhs.home_host && lhs.local_tid == rhs.local_tid;
}

}  // namespace cxloom
