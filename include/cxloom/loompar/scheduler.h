#pragma once

#include <vector>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"
#include "cxloom/common/types.h"
#include "cxloom/loommem/runtime.h"

namespace cxloom::loompar {

class PlacementScheduler {
public:
    PlacementScheduler(CxloomConfig config, const loommem::LoomMemRuntime* loommem);

    Result<HostId> SelectHost(const ThreadPlacementHint& hint,
                              const std::vector<HostLoadSnapshot>& load_view);

private:
    CxloomConfig config_;
    const loommem::LoomMemRuntime* loommem_ {nullptr};
    std::uint64_t rr_cursor_ {0};
};

}  // namespace cxloom::loompar

