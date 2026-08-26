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
    std::size_t per_host_extent_bytes {1ULL << 28};
    std::size_t coherence_granule_bytes {4096};
    std::size_t queue_capacity_entries {1024};

    std::uintptr_t cxl_base_hint {0};
    double scheduler_slack_ratio {0.25};

    std::string instance_name {"cxloom"};
};

class ConfigValidator {
public:
    static Status Validate(const CxloomConfig& config);
};

}  // namespace cxloom
