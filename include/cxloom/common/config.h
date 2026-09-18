#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "cxloom/common/status.h"
#include "cxloom/common/types.h"

namespace cxloom {

enum class PlacementPolicy { kMemoryAware = 0, kRoundRobin = 1, kLeastLoaded = 2 };

struct CxloomConfig {
    HostId local_host_id {0};
    std::uint16_t host_count {1};

    std::size_t shared_region_bytes {1ULL << 30};
    std::size_t coherence_granule_bytes {4096};
    // Zero selects the largest capacity up to 1024 that fits all directed
    // host-pair queues in the reserved queue region.
    std::size_t queue_capacity_entries {0};
    // Host-local immutable replica cache limits. Snapshots retained by callers
    // remain valid after their cache entry is evicted.
    std::size_t replica_cache_capacity_entries {1024};
    std::size_t replica_cache_capacity_bytes {64ULL << 20};

    // Required shared CXL device path (for example /dev/dax0.0).
    // Tests may use a regular file with the same shared mapping path.
    std::string shared_region_path;
    bool bootstrap_owner {false};
    bool create_region_file {false};
    std::uint64_t bootstrap_timeout_ms {10000};

    std::uintptr_t cxl_base_hint {0};
    PlacementPolicy placement_policy {PlacementPolicy::kMemoryAware};
    double scheduler_slack_ratio {0.25};
    // Zero means unlimited. Limits are enforced independently on each execution host.
    std::uint32_t max_running_threads_per_host {0};
    std::uint32_t max_pending_creates_per_host {0};
    double scheduler_load_half_life_ms {250.0};
    double scheduler_queue_weight {1.0};
    double scheduler_history_weight {0.5};
    double scheduler_remote_locality_penalty {2.0};

    std::string instance_name {"cxloom"};
};

class ConfigValidator {
  public:
    static Status Validate(const CxloomConfig& config);
};

}  // namespace cxloom
