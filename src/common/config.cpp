#include "cxloom/common/config.h"

#include "cxloom/common/status.h"

namespace cxloom {

Status ConfigValidator::Validate(const CxloomConfig& config) {
    if (config.host_count == 0) {
        return Status::InvalidArgument("host_count must be greater than zero");
    }
    if (config.local_host_id >= config.host_count) {
        return Status::InvalidArgument("local_host_id must be smaller than host_count");
    }
    if (config.host_count > kMaxHosts) {
        return Status::InvalidArgument("host_count exceeds the bootstrap host table capacity");
    }
    if (config.shared_region_bytes == 0) {
        return Status::InvalidArgument("shared region size must be non-zero");
    }
    if (config.coherence_granule_bytes == 0) {
        return Status::InvalidArgument("coherence_granule_bytes must be non-zero");
    }
    if (config.bootstrap_timeout_ms == 0) {
        return Status::InvalidArgument("bootstrap_timeout_ms must be non-zero");
    }
    return Status::Ok();
}

}  // namespace cxloom
