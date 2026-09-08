#include "cxloom/loompar/scheduler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace cxloom::loompar {
namespace {
std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

PlacementScheduler::PlacementScheduler(CxloomConfig config, const loommem::LoomMemRuntime* loommem)
    : config_(std::move(config)), loommem_(loommem) {}

Result<HostId> PlacementScheduler::SelectHost(const ThreadPlacementHint& hint,
                                              const std::vector<HostLoadSnapshot>& load_view) {
    if (hint.has_explicit_host) return hint.explicit_host;
    if (load_view.empty()) return Status::FailedPrecondition("scheduler has no host load samples");

    const auto now = NowNs();
    double min_score = std::numeric_limits<double>::infinity();
    HostId best = load_view.front().host_id;
    bool eligible = false;
    HostId preferred = std::numeric_limits<HostId>::max();
    if (hint.has_dominant_gptr && loommem_ != nullptr) {
        auto owner = loommem_->ResolvePreferredHost(hint.dominant_gptr);
        if (owner.ok()) preferred = owner.value();
    }
    for (const auto& snapshot : load_view) {
        if (config_.max_running_threads_per_host && snapshot.running_threads >= config_.max_running_threads_per_host)
            continue;
        if (config_.max_pending_creates_per_host && snapshot.pending_creates >= config_.max_pending_creates_per_host)
            continue;
        eligible = true;
        const auto age_ms = snapshot.sampled_at_ns && now > snapshot.sampled_at_ns
            ? static_cast<double>(now - snapshot.sampled_at_ns) / 1.0e6 : 0.0;
        const auto half_life = std::max(1.0, config_.scheduler_load_half_life_ms);
        const auto freshness = std::exp2(-age_ms / half_life);
        const auto running = static_cast<double>(snapshot.running_threads) * freshness;
        const auto pending = static_cast<double>(snapshot.pending_creates) * freshness;
        const auto queue = static_cast<double>(snapshot.queued_messages) * config_.scheduler_queue_weight;
        double history = 0.0;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            const auto launches = launches_.count(snapshot.host_id) ? launches_.at(snapshot.host_id) : 0;
            const auto completions = completions_.count(snapshot.host_id) ? completions_.at(snapshot.host_id) : 0;
            // A host repeatedly selected without completing work is penalized;
            // completions provide a small warm-cache/locality reward.
            history = static_cast<double>(launches > completions ? launches - completions : 0) * config_.scheduler_history_weight;
        }
        double locality = 0.0;
        if (preferred != std::numeric_limits<HostId>::max() && snapshot.host_id != preferred)
            locality = config_.scheduler_remote_locality_penalty;
        const auto score = running + pending + queue + history + locality;
        if (score < min_score) {
            min_score = score;
            best = snapshot.host_id;
        }
    }
    if (!eligible) return Status::Unavailable("all execution hosts are at capacity");
    return best;
}

void PlacementScheduler::RecordLaunch(HostId host) {
    std::lock_guard<std::mutex> lock(history_mutex_);
    ++launches_[host];
}
void PlacementScheduler::RecordCompletion(HostId host) {
    std::lock_guard<std::mutex> lock(history_mutex_);
    ++completions_[host];
}
std::uint64_t PlacementScheduler::launch_count(HostId host) const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    auto it = launches_.find(host);
    return it == launches_.end() ? 0 : it->second;
}
}  // namespace cxloom::loompar
