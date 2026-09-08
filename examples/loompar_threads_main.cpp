#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include "cxloom/loompar/runtime.h"

using namespace cxloom;
namespace {
constexpr unsigned kHosts = 16;
std::atomic<std::uint64_t> executed{0};
HostId local_host;
loompar::LoomParRuntime* runtime;
struct Argument { std::uint64_t value; std::uint16_t target; std::uint16_t depth; };
void Require(Status status) {
    if (!status.ok()) { std::cerr << status.message() << std::endl; std::exit(1); }
}
unsigned Setting(const char* name, unsigned fallback, unsigned maximum) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    auto parsed = std::strtoul(value, &end, 10);
    if (!*value || *end || parsed > maximum) { std::cerr << "invalid " << name << std::endl; std::exit(2); }
    return static_cast<unsigned>(parsed);
}
Result<GlobalThreadId> Create(HostId target, unsigned depth) {
    ThreadPlacementHint placement;
    placement.has_explicit_host = true;
    placement.explicit_host = target;
    Argument argument{0xc001cafeULL, target, static_cast<std::uint16_t>(depth)};
    std::vector<std::byte> args(sizeof(argument));
    std::memcpy(args.data(), &argument, sizeof(argument));
    return runtime->CreateThread("worker-v1", std::move(args), placement);
}
void Worker(void* data) {
    Argument argument{};
    std::memcpy(&argument, data, sizeof(argument));
    if (argument.target != local_host || argument.value != 0xc001cafeULL || argument.depth > 1) throw 1;
    if (argument.depth) {
        auto child = Create((local_host + 1) % kHosts, 0);
        if (!child.ok() || !runtime->JoinThread(child.value()).ok()) throw 2;
    }
    ++executed;
}
void Missing(void*) {}
void Phase(loommem::LoomMemRuntime& mem, std::uint64_t phase) {
    Require(mem.PublishBootstrapProbe(phase));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (HostId host = 0; host < kHosts; ++host) {
        for (;;) {
            auto probe = mem.ReadBootstrapProbe(host);
            if (probe.ok() && probe.value() >= phase) break;
            if (std::chrono::steady_clock::now() >= deadline) { std::cerr << "phase timeout host=" << host << std::endl; std::exit(1); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
}
int main() {
    const char* path = std::getenv("CL_DAX_DEVICE");
    if (!path) { std::cerr << "CL_DAX_DEVICE required\n"; return 2; }
    CxloomConfig config;
    config.host_count = Setting("CL_HOST_COUNT", kHosts, kHosts);
    if (config.host_count != kHosts) { std::cerr << "acceptance requires 16 hosts\n"; return 2; }
    config.local_host_id = Setting("CL_HOST_ID", 0, kHosts - 1);
    local_host = config.local_host_id;
    const unsigned rounds = Setting("CL_PAR_ROUNDS", 300, 1000000);
    const unsigned creators = Setting("CL_PAR_CREATORS", 4, 32);
    const unsigned delay_ms = Setting("CL_PAR_ROUND_DELAY_MS", 1000, 60000);
    if (!rounds || !creators) return 2;
    config.shared_region_path = path;
    config.shared_region_bytes = 192ULL << 20;
    if (const char* bytes = std::getenv("CL_SHARED_REGION_BYTES")) {
        char* end = nullptr;
        config.shared_region_bytes = std::strtoull(bytes, &end, 10);
        if (std::string(end) == "G") config.shared_region_bytes <<= 30;
        else if (std::string(end) == "M") config.shared_region_bytes <<= 20;
        else if (*end) { std::cerr << "invalid shared region size\n"; return 2; }
    }
    config.bootstrap_owner = local_host == 0;
    config.create_region_file = std::getenv("CL_CREATE_REGION_FILE") != nullptr;
    config.queue_capacity_entries = 2;
    loommem::LoomMemRuntime mem(config);
    Require(mem.Initialize());
    std::cout << "region ready host=" << local_host << std::endl;
    loompar::LoomParRuntime par(config, &mem);
    runtime = &par;
    Require(par.Initialize());
    if (local_host == 0) Require(par.RegisterFunction("home-only", Missing).status());
    Require(par.RegisterFunction("worker-v1", Worker).status());
    Phase(mem, 1);
    const auto started = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round) {
        std::vector<std::thread> producers;
        for (unsigned creator = 0; creator < creators; ++creator) {
            producers.emplace_back([creator] {
                for (unsigned peer = 1; peer < kHosts; ++peer) {
                    // Each creator reaches every other host; each remote worker creates and joins a child.
                    const auto target = static_cast<HostId>((local_host + 1 + (peer - 1 + creator) % (kHosts - 1)) % kHosts);
                    auto id = Create(target, 1);
                    Require(id.status());
                    Require(runtime->JoinThread(id.value()));
                }
            });
        }
        for (auto& producer : producers) producer.join();
        Phase(mem, 2ULL + 2ULL * round);
        const auto expected = static_cast<std::uint64_t>(round + 1) * creators * (kHosts - 1) * 2;
        if (executed != expected || par.thread_manager().size() != 0) {
            std::cerr << "count/reclamation mismatch host=" << local_host << " expected=" << expected << " actual=" << executed << std::endl;
            return 1;
        }
        // Separate verification from the next round, which could otherwise change local counters.
        Phase(mem, 3ULL + 2ULL * round);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if ((round + 1) % 10 == 0) std::cout << "progress host=" << local_host << " rounds=" << round + 1 << std::endl;
    }
    if (local_host == 0) {
        ThreadPlacementHint hint; hint.has_explicit_host = true; hint.explicit_host = 1;
        auto missing = par.CreateThread("home-only", {}, hint);
        Require(missing.status());
        if (par.JoinThread(missing.value()).ok()) return 1;
    }
    Phase(mem, 2ULL * rounds + 4);
    Require(par.Finalize());
    Require(mem.Finalize());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "PASS host=" << local_host << " hosts=16 rounds=" << rounds << " creators=" << creators
              << " executed=" << executed << " home_records=0 nested=yes elapsed_ms=" << elapsed << std::endl;
}
