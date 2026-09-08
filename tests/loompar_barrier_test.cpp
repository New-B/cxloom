#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include "cxloom/loompar/runtime.h"

#define CHECK(x) do { if (!(x)) { std::cerr << "failed line " << __LINE__ << ": " #x << std::endl; std::abort(); } } while (0)
using namespace cxloom;
int main() {
    loompar::BarrierManager barrier;
    CHECK(!barrier.Wait(1, 0).ok());
    auto first = std::async(std::launch::async, [&] { return barrier.Wait(1, 2); });
    while (!barrier.Query(1).ok()) std::this_thread::yield();
    CHECK(first.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout);
    CHECK(!barrier.idle());
    CHECK(barrier.Wait(1, 2).ok()); CHECK(first.get().ok());
    CHECK(barrier.Query(1).value().generation == 1);
    auto second = std::async(std::launch::async, [&] { return barrier.Wait(1, 2); });
    CHECK(second.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout);
    CHECK(barrier.Wait(1, 2).ok()); CHECK(second.get().ok());
    CHECK(barrier.Query(1).value().generation == 2);
    auto mismatch = std::async(std::launch::async, [&] { return barrier.Wait(2, 2); });
    while (!barrier.Query(2).ok()) std::this_thread::yield();
    CHECK(!barrier.Wait(2, 3).ok()); CHECK(!mismatch.get().ok());
    CHECK(!barrier.Wait(2, 2).ok());
    CHECK(barrier.Wait(3, 1).ok()); // A broken ID does not affect other IDs.

    // Coordinator must tolerate arrivals before its local participants register.
    unsigned releases = 0;
    loompar::BarrierManager world(0, 16, [&](MessageKind kind, HostId, std::uint64_t, std::uint64_t generation, bool failed) {
        CHECK(kind == MessageKind::kBarrierRelease && generation == 0 && !failed); ++releases;
    });
    for (HostId host = 1; host < 16; ++host) {
        CHECK(world.Handle(MessageKind::kBarrierArrive, host, 8, 0, false).ok());
        CHECK(world.Handle(MessageKind::kBarrierArrive, host, 8, 0, false).ok());
    }
    CHECK(releases == 0);
    CHECK(world.Wait(8, 1).ok()); CHECK(releases == 15);
    CHECK(world.Handle(MessageKind::kBarrierArrive, 1, 8, 0, false).ok());
    CHECK(world.Query(8).value().generation == 1);

    // Finalize cannot remove transport while an application thread is in barrier.
    CxloomConfig config; config.shared_region_bytes = 192ULL << 20;
    loommem::LoomMemRuntime mem(config); CHECK(mem.Initialize().ok());
    loompar::LoomParRuntime par(config, &mem); CHECK(par.Initialize().ok());
    auto waiting = std::async(std::launch::async, [&] { return par.Barrier(7, 2); });
    CHECK(waiting.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout);
    CHECK(!par.Finalize().ok());
    CHECK(par.Barrier(7, 2).ok()); CHECK(waiting.get().ok());
    CHECK(par.Finalize().ok()); CHECK(mem.Finalize().ok());
    std::cout << "blocking generations, mismatched counts, early/duplicate arrivals and finalize protection passed\n";
}
