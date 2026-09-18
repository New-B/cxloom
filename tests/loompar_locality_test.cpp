#include "shared_region_fixture.h"
#include "cxloom/loompar/runtime.h"
#include <future>
#include <iostream>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed " << __LINE__ << ": " #x << std::endl; std::abort(); } } while (0)
using namespace cxloom;
int main() {
    CxloomConfig config;
    config.host_count = 2;
    config.shared_region_bytes = 192ULL << 20;
    config.scheduler_queue_weight = config.scheduler_history_weight = 0;
    config.scheduler_remote_locality_penalty = 100;
    config.replica_cache_capacity_entries = 2;
    SharedRegionFixture region(config);
    loommem::LoomMemRuntime a(config);
    CHECK(a.Initialize().ok());
    loompar::LoomParRuntime pa(config, &a);
    CHECK(pa.Initialize().ok());
    auto other = config;
    other.local_host_id = 1;
    other.bootstrap_owner = other.create_region_file = false;
    loommem::LoomMemRuntime b(other);
    CHECK(b.Initialize().ok());
    loompar::LoomParRuntime pb(other, &b);
    CHECK(pb.Initialize().ok());
    auto registered = std::async(std::launch::async, [&] {
        return pa.RegisterClusterFunctions({{"host", 1, 0, 1, 1, [](void*) {
            loompar::ThreadManager::SetCurrentResult(10);
        }}}, 3000);
    });
    CHECK(pb.RegisterClusterFunctions({{"host", 1, 0, 1, 1, [](void*) {
        loompar::ThreadManager::SetCurrentResult(11);
    }}}, 3000).ok());
    CHECK(registered.get().ok());
    auto allocation = a.AllocateShared(loommem::AllocationOptions{192, 64, 64});
    CHECK(allocation.ok());
    auto object = allocation.value();
    WorkingSetEntry entry{object, 0, 64, MemoryAccess::kWrite};
    auto query = a.QueryLocality(entry);
    CHECK(query.ok() && query.value()[0].token_owner == 0 && query.value()[0].last_writer == kMaxHosts);
    auto write = b.AcquireWriteRange(object, 0, 64, 3000);
    CHECK(write.ok());
    CHECK(b.AbortWriteBuffer(write.value()).ok());
    query = a.QueryLocality(entry);
    CHECK(query.ok() && query.value()[0].token_owner == 1 && query.value()[0].last_writer == kMaxHosts);
    loompar::PlacementScheduler scheduler(config, &a);
    ThreadPlacementHint hint;
    hint.working_set = {entry};
    std::vector<HostLoadSnapshot> loads{{0, 0, 0}, {1, 0, 0}};
    CHECK(scheduler.SelectHost(hint, loads).value() == 1);
    auto thread = pa.CreateRegisteredThread(1, {}, hint);
    CHECK(thread.ok());
    uint64_t result = 0;
    CHECK(pa.JoinThread(thread.value(), &result).ok() && result == 11);
    CHECK(pa.thread_manager().size() == 0);
    write = b.AcquireWriteRange(object, 0, 64, 3000);
    CHECK(write.ok() && b.ReleaseWriteBuffer(write.value()).ok());
    CHECK(a.AcquireReadRange(object, 0, 64, 3000).ok());
    query = b.QueryLocality(entry);
    CHECK(query.ok() && query.value()[0].last_writer == 1 && query.value()[0].current_replica_hosts == 1);
    hint.working_set[0].access = MemoryAccess::kRead;
    CHECK(scheduler.SelectHost(hint, loads).value() == 0);
    thread = pb.CreateRegisteredThread(1, {}, hint);
    CHECK(thread.ok() && pb.JoinThread(thread.value(), &result).ok() && result == 10);
    CHECK(pb.thread_manager().size() == 0);
    // Publication invalidates freshness remotely without invalidating old snapshots.
    write = b.AcquireWriteRange(object, 0, 64, 3000);
    CHECK(write.ok() && b.ReleaseWriteBuffer(write.value()).ok());
    query = a.QueryLocality(entry);
    CHECK(query.ok() && query.value()[0].current_replica_hosts == 0 && query.value()[0].version == 2);
    CHECK(scheduler.SelectHost(hint, loads).value() == 1);
    CHECK(a.SynchronizeAcquire().ok());
    CHECK(a.AcquireReadRange(object, 0, 64, 3000).ok());
    CHECK(a.QueryLocality(entry).value()[0].current_replica_hosts == 1);
    CHECK(a.AcquireReadRange(object, 64, 64, 3000).ok());
    CHECK(a.AcquireReadRange(object, 128, 64, 3000).ok());
    CHECK(a.QueryLocality(entry).value()[0].current_replica_hosts == 0); // actual LRU eviction
    hint.working_set = {{object, 0, 64, MemoryAccess::kWrite, 1},
                        {object, 64, 64, MemoryAccess::kWrite, 4}};
    CHECK(scheduler.SelectHost(hint, loads).value() == 0);
    hint.working_set[0].weight = 8;
    CHECK(scheduler.SelectHost(hint, loads).value() == 1);
    loads[1].running_threads = 1000;
    CHECK(scheduler.SelectHost(hint, loads).value() == 0);
    // Staged publication must precede the final locality decision: before
    // publication last_writer is host 1; afterwards the read should run on 0.
    write = a.AcquireWriteRange(object, 0, 64, 3000);
    CHECK(write.ok() && a.StageWriteBuffer(&write.value()).ok());
    hint.working_set = {{object, 0, 64, MemoryAccess::kRead, 1}};
    CHECK(a.QueryLocality(entry).value()[0].last_writer == 1);
    thread = pa.CreateRegisteredThread(1, {}, hint);
    CHECK(thread.ok() && pa.JoinThread(thread.value(), &result).ok() && result == 10);
    CHECK(a.QueryLocality(entry).value()[0].last_writer == 0);
    hint.working_set[0].weight = std::numeric_limits<double>::quiet_NaN();
    CHECK(!scheduler.SelectHost(hint, loads).ok());
    entry.offset = 192;
    CHECK(!a.QueryLocality(entry).ok());
    CHECK(a.InvalidateReadCache(object).ok());
    CHECK(pa.Finalize().ok() && pb.Finalize().ok());
    CHECK(a.Finalize().ok() && b.Finalize().ok());
    std::cout << "actual token/writer/replica locality, weights, overload and remote launch/join passed\n";
}
