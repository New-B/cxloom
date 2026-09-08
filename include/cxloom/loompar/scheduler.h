#pragma once

#include <vector>
#include <mutex>
#include <unordered_map>

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
    void RecordLaunch(HostId host);
    void RecordCompletion(HostId host);
    std::uint64_t launch_count(HostId host) const;

private:
    CxloomConfig config_;
    const loommem::LoomMemRuntime* loommem_ {nullptr};
    std::uint64_t rr_cursor_ {0};
    HostId last_selected_ {0};
    mutable std::mutex history_mutex_;
    std::unordered_map<HostId, std::uint64_t> launches_;
    std::unordered_map<HostId, std::uint64_t> completions_;
};

}  // namespace cxloom::loompar
