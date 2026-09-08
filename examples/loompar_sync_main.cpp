#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include "cxloom/loompar/runtime.h"

using namespace cxloom;
namespace {
constexpr unsigned kHosts = 16;
loommem::LoomMemRuntime* memory;
void Require(Status status) {
    if (!status.ok()) { std::cerr << status.message() << std::endl; std::exit(1); }
}
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << std::endl; std::exit(1); }
}
std::uint64_t Value(const loommem::ReadSnapshot& snapshot) {
    std::uint64_t value;
    std::memcpy(&value, snapshot.data(), sizeof(value));
    return value;
}
void Stage(GlobalPointer object, unsigned slot, std::uint64_t value) {
    auto write = memory->AcquireWriteRange(object, slot * 64, sizeof(value), 10000);
    Require(write.status());
    std::memcpy(write.value().data(), &value, sizeof(value));
    Require(memory->StageWriteBuffer(&write.value()));
    Check(!write.value().storage, "stage did not transfer ownership");
}
struct Invocation { GlobalPointer object; std::uint64_t fail; };
void Worker(void* bytes) {
    Invocation invocation{};
    std::memcpy(&invocation, bytes, sizeof(invocation));
    auto read = memory->AcquireReadRange(invocation.object, 0, 8, 10000);
    if (!read.ok() || Value(read.value()) != 11) throw 1;
    Stage(invocation.object, 0, 22);
    if (invocation.fail) throw 2;
}
void Quiesce(loommem::LoomMemRuntime& mem) {
    Require(mem.PublishBootstrapProbe(1));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (HostId host = 0; host < kHosts; ++host) {
        for (;;) {
            auto value = mem.ReadBootstrapProbe(host);
            if (value.ok() && value.value() == 1) break;
            Check(std::chrono::steady_clock::now() < deadline, "quiescence timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
}
int main() {
    const auto* path = std::getenv("CL_DAX_DEVICE");
    Check(path, "CL_DAX_DEVICE required");
    CxloomConfig config;
    config.host_count = kHosts;
    Check(!std::getenv("CL_HOST_COUNT") || std::string(std::getenv("CL_HOST_COUNT")) == "16", "requires 16 hosts");
    config.local_host_id = std::getenv("CL_HOST_ID") ? std::strtoul(std::getenv("CL_HOST_ID"), nullptr, 10) : 0;
    Check(config.local_host_id < kHosts, "invalid host");
    config.shared_region_path = path;
    config.shared_region_bytes = 192ULL << 20;
    // Existing container launch script exports the capacity as 1G by default.
    if (const auto* bytes = std::getenv("CL_SHARED_REGION_BYTES")) {
        char* end = nullptr;
        config.shared_region_bytes = std::strtoull(bytes, &end, 10);
        if (std::string(end) == "G") config.shared_region_bytes <<= 30;
        else if (std::string(end) == "M") config.shared_region_bytes <<= 20;
        else Check(!*end, "invalid shared region size");
    }
    config.bootstrap_owner = config.local_host_id == 0;
    config.create_region_file = std::getenv("CL_CREATE_REGION_FILE") != nullptr;
    config.queue_capacity_entries = 2;
    loommem::LoomMemRuntime mem(config);
    Require(mem.Initialize());
    memory = &mem;
    std::cout << "region ready host=" << config.local_host_id << std::endl;
    loompar::LoomParRuntime par(config, &mem);
    Require(par.Initialize());
    Require(par.RegisterFunction("sync-worker-v1", Worker).status());
    loommem::AllocationOptions options{128, 64, CoherenceGranularity::kFixedBlock, 64};
    auto object = mem.AllocateShared(options);
    Require(object.status());
    Stage(object.value(), 0, 0);
    Stage(object.value(), 1, 0);
    Require(mem.SynchronizeRelease());
    Require(mem.PublishSharedObject(object.value(), 128));
    Require(mem.WaitForAllSharedObjects(30000));
    std::vector<GlobalPointer> objects;
    for (HostId host = 0; host < kHosts; ++host) {
        auto published = mem.ReadPublishedSharedObject(host);
        Require(published.status()); objects.push_back(published.value().gptr);
    }
    Require(par.Barrier(1, 1));
    if (config.local_host_id == 0) {
        for (unsigned test = 0; test < 3; ++test) {
            Stage(object.value(), 0, 7);
            Require(mem.SynchronizeRelease());
            auto old = mem.AcquireReadRange(object.value(), 0, 8, 10000);
            Require(old.status());
            Stage(object.value(), 0, 11);
            Invocation invocation{object.value(), test == 2 ? 1ULL : 0ULL};
            std::vector<std::byte> args(sizeof(invocation));
            std::memcpy(args.data(), &invocation, sizeof(invocation));
            ThreadPlacementHint hint; hint.has_explicit_host = true; hint.explicit_host = test == 0 ? 0 : 1;
            auto id = par.CreateThread("sync-worker-v1", args, hint);
            Require(id.status());
            const auto joined = par.JoinThread(id.value());
            Check(joined.ok() == (test != 2), "unexpected join result");
            Check(mem.cached_replica_count() == 0, "join failed to invalidate cached replicas");
            auto fresh = mem.AcquireReadRange(object.value(), 0, 8, 10000);
            Require(fresh.status());
            Check(Value(fresh.value()) == 22 && Value(old.value()) == 7, "create/join publication or immutable snapshot failed");
        }
        Stage(object.value(), 0, 0); Require(mem.SynchronizeRelease());
    }
    Require(par.Barrier(100, 1));
    const unsigned participants = 1 + config.local_host_id % 2;
    constexpr unsigned rounds = 24;
    std::vector<std::thread> workers;
    for (unsigned slot = 0; slot < participants; ++slot) {
        workers.emplace_back([&, slot] {
            for (unsigned round = 1; round <= rounds; ++round) {
                if (config.local_host_id == 15 && slot == 1 && round == 1)
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                Stage(object.value(), slot, round * 1000 + config.local_host_id * 10 + slot);
                Require(par.Barrier(200, participants));
                for (HostId host = 0; host < kHosts; ++host) {
                    for (unsigned lane = 0; lane < 1U + host % 2; ++lane) {
                        auto read = mem.AcquireReadRange(objects[host], lane * 64, 8, 10000);
                        Require(read.status());
                        Check(Value(read.value()) == round * 1000 + host * 10 + lane, "barrier released early or read a stale generation");
                    }
                }
                Require(par.Barrier(201, participants));
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Require(par.Barrier(300, 1));
    Check(mem.staged_write_count() == 0, "staged writes leaked");
    Require(mem.FreeShared(object.value()));
    if (config.local_host_id == 0) {
        std::thread conflicting([&] { Check(!par.Barrier(999, 2).ok(), "mismatched barrier succeeded"); });
        Check(!par.Barrier(999, 3).ok(), "mismatched barrier succeeded");
        conflicting.join();
    } else {
        Check(!par.Barrier(999, 1).ok(), "remote barrier failure did not propagate");
    }
    Require(par.Barrier(1000, 1)); // The failed ID must not stop the poller or other barriers.
    Quiesce(mem); // Keep pollers alive until all retirement messages have completed.
    Require(par.Finalize());
    Require(mem.Finalize());
    std::cout << "PASS host=" << config.local_host_id << " hosts=16 barrier_rounds=" << rounds
              << " local_participants=" << participants << " sync=create/start/complete/join/barrier" << std::endl;
}
