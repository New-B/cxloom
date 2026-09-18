#include "cxloom/loompar/threading.h"
#include "cxloom/loompar/scheduler.h"
#include <atomic>
#include <chrono>
#include <iostream>
using namespace cxloom;
using namespace cxloom::loompar;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed " << __LINE__ << ": " #x << std::endl; std::abort(); } } while (0)
template<class F> void Eventually(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
}
int main() {
    ThreadManager manager(0);
    Condition condition;
    std::mutex mutex;
    bool wake = false;
    std::vector<GlobalThreadId> ids;
    const auto workers = manager.SampleExecutionLoad().workers;
    CHECK(workers >= 2);
    for (unsigned i = 0; i < workers + 3; ++i) {
        auto id = manager.AllocateThread(0, 1, {}); CHECK(id.ok());
        CHECK(manager.MarkLaunching(id.value()).ok());
        CHECK(manager.Launch(id.value(), [&](void*) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return wake; });
        }).ok());
        ids.push_back(id.value());
    }
    Eventually([&] { auto l = manager.SampleExecutionLoad(); return l.blocked == ids.size() && l.executing == 0 && l.ready == 0; });
    { std::lock_guard<std::mutex> lock(mutex); wake = true; condition.notify_all(); }
    for (auto id : ids) CHECK(manager.Join(id).ok());
    ids.clear();
    std::atomic<bool> finish{false};
    for (unsigned i = 0; i < workers + 2; ++i) {
        auto id = manager.AllocateThread(0, 1, {}); CHECK(id.ok());
        CHECK(manager.MarkLaunching(id.value()).ok());
        CHECK(manager.Launch(id.value(), [&](void*) { while (!finish) std::this_thread::yield(); }).ok());
        ids.push_back(id.value());
    }
    Eventually([&] { auto l = manager.SampleExecutionLoad(); return l.executing == workers && l.ready == 2 && l.blocked == 0; });
    finish = true;
    for (auto id : ids) CHECK(manager.Join(id).ok());
    CHECK(manager.SampleExecutionLoad().executing == 0 && manager.SampleExecutionLoad().ready == 0);

    CxloomConfig config; config.host_count = 3; config.placement_policy = PlacementPolicy::kRoundRobin;
    config.max_running_threads_per_host = 10;
    PlacementScheduler rr(config, nullptr);
    std::vector<HostLoadSnapshot> samples{{2, 0}, {0, 0}, {1, 10}};
    CHECK(rr.SelectHost({}, samples, false).value() == 0);
    CHECK(rr.SelectHost({}, samples, false).value() == 0);
    for (int i = 0; i < 6; ++i) CHECK(rr.SelectHost({}, samples).value() == (i % 2 ? 2 : 0));
    ThreadPlacementHint ignored_working_set;
    ignored_working_set.working_set.push_back({{99, 99}, 0, 64, MemoryAccess::kRead, 1});
    CHECK(rr.SelectHost(ignored_working_set, samples).ok());
    ThreadPlacementHint explicit_hint; explicit_hint.has_explicit_host = true; explicit_hint.explicit_host = 1;
    CHECK(rr.SelectHost(explicit_hint, samples).status().code() == StatusCode::kUnavailable);
    samples[1].running_threads = samples[0].running_threads = 10;
    CHECK(rr.SelectHost({}, samples).status().code() == StatusCode::kUnavailable);
    config.placement_policy = PlacementPolicy::kLeastLoaded;
    config.max_running_threads_per_host = 0;
    PlacementScheduler least(config, nullptr);
    samples = {{0, 100}, {1, 4}};
    samples[0].execution = {0, 0, 100, 4};
    samples[1].execution = {4, 0, 0, 4};
    CHECK(least.SelectHost({}, samples).value() == 0); // parked calls are not CPU demand
    samples[0].execution = {2, 0, 0, 2};
    samples[1].execution = {4, 0, 0, 8};
    CHECK(least.SelectHost({}, samples).value() == 1); // normalize by workers
    samples[1].execution.ready = 8;
    CHECK(least.SelectHost({}, samples).value() == 0);
    samples[0].sampled_at_ns = 1; // old busy sample must not decay into idle
    samples[1].execution.ready = 0;
    CHECK(least.SelectHost({}, samples).value() == 1);
    samples = {{0, 0}, {1, 0}};
    CHECK(least.SelectHost({}, samples).value() == 0);
    least.RecordLaunch(0);
    CHECK(least.SelectHost({}, samples).value() == 1); // predict before telemetry catches up
    config.placement_policy = PlacementPolicy::kMemoryAware;
    PlacementScheduler memory(config, nullptr);
    CHECK(memory.SelectHost(ignored_working_set, samples).status().code() == StatusCode::kFailedPrecondition);
    samples[0].running_threads = 100;
    samples[0].execution = {0, 0, 100, 4};
    for (int i = 0; i < 100; ++i) memory.RecordLaunch(0);
    CHECK(memory.SelectHost({}, samples).value() == 0); // history does not recount blocked calls
    std::cout << "fiber execution telemetry and placement policies passed\n";
}
