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
                                              const std::vector<HostLoadSnapshot>& load_view, bool advance) {
    std::lock_guard<std::mutex> selection_lock(selection_mutex_);
    if (config_.placement_policy != PlacementPolicy::kMemoryAware &&
        config_.placement_policy != PlacementPolicy::kRoundRobin &&
        config_.placement_policy != PlacementPolicy::kLeastLoaded)
        return Status::InvalidArgument("invalid placement policy");
    if (hint.has_explicit_host && hint.explicit_host >= config_.host_count)
        return Status::InvalidArgument("explicit host out of range");
    if (load_view.empty()) return Status::FailedPrecondition("scheduler has no host load samples");

    const auto now = NowNs();
    double min_score = std::numeric_limits<double>::infinity();
    HostId best = load_view.front().host_id;
    bool eligible = false;
    std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
    std::vector<WorkingSetEntry> entries;
    if (config_.placement_policy == PlacementPolicy::kMemoryAware && !hint.has_explicit_host) {
        entries = hint.working_set;
        if (entries.empty() && hint.has_dominant_gptr)
            entries.push_back({hint.dominant_gptr, 0, 0, MemoryAccess::kReadWrite});
        if (entries.size() > 256) return Status::InvalidArgument("too many working set entries");
    }
    std::vector<double> memory_cost(config_.host_count, 0.0);
    double total_weight = 0;
    for (const auto& entry : entries) {
        if (!std::isfinite(entry.weight) || entry.weight <= 0 || entry.weight > 1e6 ||
            (entry.access != MemoryAccess::kRead && entry.access != MemoryAccess::kWrite && entry.access != MemoryAccess::kReadWrite))
            return Status::InvalidArgument("invalid working set access or weight");
        if (!loommem_) return Status::FailedPrecondition("working set requires LoomMem");
        auto blocks = loommem_->QueryLocality(entry);
        if (!blocks.ok()) return blocks.status();
        for (const auto& block : blocks.value()) {
            const double weight = entry.weight * static_cast<double>(block.bytes);
            total_weight += weight;
            for (HostId host = 0; host < config_.host_count; ++host) {
                double cost = 0;
                if (entry.access != MemoryAccess::kWrite) {
                    if (!(block.current_replica_hosts & (1ULL << host)))
                        cost += host == block.last_writer ? 0.75 : 1.0;
                }
                if (entry.access != MemoryAccess::kRead && host != block.token_owner) cost += 1.0;
                memory_cost[host] += weight * cost;
            }
        }
    }
    for (const auto& snapshot : load_view) {
        if (snapshot.host_id >= config_.host_count) return Status::InvalidArgument("load host out of range");
        if (hint.has_explicit_host && snapshot.host_id != hint.explicit_host) continue;
        if (config_.max_running_threads_per_host && snapshot.running_threads >= config_.max_running_threads_per_host)
            continue;
        if (config_.max_pending_creates_per_host && snapshot.pending_creates >= config_.max_pending_creates_per_host)
            continue;
        eligible = true;
        const auto age_ms = snapshot.sampled_at_ns && now > snapshot.sampled_at_ns
            ? static_cast<double>(now - snapshot.sampled_at_ns) / 1.0e6 : 0.0;
        const auto half_life = std::max(1.0, config_.scheduler_load_half_life_ms);
        const auto freshness = std::exp2(-age_ms / half_life);
        // Detailed samples retain observed runnable demand until refreshed;
        // age alone must not turn a long-running task into an idle worker.
        const auto running = snapshot.execution.workers
            ? static_cast<double>(snapshot.execution.executing + snapshot.execution.ready) / snapshot.execution.workers
            : static_cast<double>(snapshot.running_threads) * freshness;
        const auto pending = static_cast<double>(snapshot.pending_creates) *
            (snapshot.execution.workers ? 1.0 / snapshot.execution.workers : freshness);
        const auto queue = static_cast<double>(snapshot.queued_messages) * config_.scheduler_queue_weight;
        double history = 0.0;
        double unobserved_load = 0.0;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            const auto launches = launches_.count(snapshot.host_id) ? launches_.at(snapshot.host_id) : 0;
            const auto completions = completions_.count(snapshot.host_id) ? completions_.at(snapshot.host_id) : 0;
            const auto outstanding = launches > completions ? launches - completions : 0;
            // Predict only launches not yet covered by the active-call sample;
            // blocked calls already in telemetry must not look CPU-busy again.
            const auto unseen = snapshot.execution.workers
                ? (outstanding > snapshot.running_threads ? outstanding - snapshot.running_threads : 0)
                : outstanding;
            unobserved_load = static_cast<double>(unseen) /
                (snapshot.execution.workers ? snapshot.execution.workers : 1U);
            history = static_cast<double>(unseen) * config_.scheduler_history_weight;
        }
        const double locality = total_weight ? config_.scheduler_remote_locality_penalty *
            memory_cost[snapshot.host_id] / total_weight : 0.0;
        const auto score = config_.placement_policy == PlacementPolicy::kLeastLoaded
            ? running + pending + unobserved_load
            : running + pending + queue + history + locality;
        const auto distance = (snapshot.host_id + config_.host_count - rr_cursor_ % config_.host_count) % config_.host_count;
        const bool choose = config_.placement_policy == PlacementPolicy::kRoundRobin
            ? distance < best_distance : score < min_score;
        if (choose) {
            best_distance = distance;
            min_score = score;
            best = snapshot.host_id;
        }
    }
    if (!eligible) return Status::Unavailable("all execution hosts are at capacity");
    if (advance && !hint.has_explicit_host && config_.placement_policy == PlacementPolicy::kRoundRobin)
        rr_cursor_ = (best + 1) % config_.host_count;
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
