#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "cxloom/common/status.h"
#include "cxloom/common/types.h"

namespace cxloom {

struct CxloomConfig {
    HostId local_host_id {0};
    std::uint16_t host_count {1};

    std::size_t shared_region_bytes {1ULL << 30};
    // Legacy process-private placement heuristic; ignored by the shared global allocator.
    std::size_t per_host_extent_bytes {1ULL << 28};
    std::size_t coherence_granule_bytes {4096};
    // Zero selects the largest capacity up to 1024 that fits all directed
    // host-pair queues in the reserved queue region.
    std::size_t queue_capacity_entries {0};
    // Host-local immutable replica cache limits. Snapshots retained by callers
    // remain valid after their cache entry is evicted.
    std::size_t replica_cache_capacity_entries {1024};
    std::size_t replica_cache_capacity_bytes {64ULL << 20};

    // Empty selects a process-private anonymous mapping for unit tests.
    // A devdax path such as /dev/dax0.0 selects a shared CXL mapping.
    std::string shared_region_path;
    bool bootstrap_owner {false};
    bool create_region_file {false};
    std::uint64_t bootstrap_timeout_ms {10000};

    std::uintptr_t cxl_base_hint {0};
    double scheduler_slack_ratio {0.25};

    std::string instance_name {"cxloom"};
};

class ConfigValidator {
  public:
    static Status Validate(const CxloomConfig& config);
};

}  // namespace cxloom
