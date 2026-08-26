#include "cxloom/loompar/scheduler.h"

#include <limits>

namespace cxloom::loompar {

PlacementScheduler::PlacementScheduler(CxloomConfig config, const loommem::LoomMemRuntime* loommem)
    : config_(std::move(config)), loommem_(loommem) {}

Result<HostId> PlacementScheduler::SelectHost(const ThreadPlacementHint& hint,
                                              const std::vector<HostLoadSnapshot>& load_view) {
    if (hint.has_explicit_host) {
        return hint.explicit_host;
    }

    if (hint.has_dominant_gptr && loommem_ != nullptr) {
        auto preferred = loommem_->ResolvePreferredHost(hint.dominant_gptr);
        if (preferred.ok()) {
            return preferred.value();
        }
    }

    if (!load_view.empty()) {
        HostId best_host = load_view.front().host_id;
        std::uint32_t best_score = std::numeric_limits<std::uint32_t>::max();
        for (const auto& snapshot : load_view) {
            const auto score = snapshot.running_threads + snapshot.pending_creates;
            if (score < best_score) {
                best_score = score;
                best_host = snapshot.host_id;
            }
        }
        return best_host;
    }

    const auto target = static_cast<HostId>((config_.local_host_id + rr_cursor_++) % config_.host_count);
    return target;
}

}  // namespace cxloom::loompar

