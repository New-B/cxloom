#include "cxloom/cxloom_mem.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
unsigned host;
cl_runtime_t* runtime = nullptr;
std::atomic<unsigned> executed{0}, remote_executed{0};
#define CHECK(x) do { if (!(x)) { std::cerr << "host=" << host << " line=" << __LINE__ << ": " << #x << std::endl; std::exit(1); } } while (0)
struct Argument { cl_gptr_t output; std::uint32_t slot; std::uint32_t creator; std::uint32_t nested; };
// Deliberately private symbols: no -rdynamic, dladdr or equal virtual addresses.
void* Leaf(void* data) {
    auto seed = *static_cast<std::uint64_t*>(data);
    return reinterpret_cast<void*>((static_cast<std::uintptr_t>(host + 1) << 32) | seed);
}
void* Worker(void* data) {
    auto args = *static_cast<Argument*>(data);
    if (args.nested) {
        std::uint64_t seed = args.slot + 1;
        cl_pthread_t child{};
        CHECK(cl_pthread_create(runtime, &child, Leaf, &seed, sizeof(seed)) == CL_OK);
        void* result = nullptr;
        CHECK(cl_pthread_join(runtime, child, &result) == CL_OK);
        CHECK((reinterpret_cast<std::uintptr_t>(result) & 0xffffffffULL) == seed);
    }
    // Keep a batch outstanding so automatic placement exercises remote hosts.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::uint64_t value = (static_cast<std::uint64_t>(host + 1) << 32) | (args.slot + 1);
    ++executed;
    if (args.creator != host) ++remote_executed;
    CHECK(cl_mem_write(runtime, args.output, args.slot * sizeof(value), &value, sizeof(value), 10000) == CL_OK);
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}
void* Unregistered(void*) { return nullptr; }
void Barrier(cl_pthread_barrier_t& barrier) { CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK); }
}
int main(int argc, char** argv) {
    host = std::strtoul(std::getenv("CL_HOST_ID"), nullptr, 10);
    const std::string mode = argc > 1 ? argv[1] : "success";
    cl_config_t config{};
    config.local_host_id = host; config.host_count = 16; config.shared_region_bytes = 192ULL << 20;
    config.coherence_granule_bytes = 4096; config.queue_capacity_entries = 2;
    config.shared_region_path = std::getenv("CL_DAX_DEVICE"); config.bootstrap_owner = host == 0;
    config.create_region_file = std::getenv("CL_CREATE_REGION_FILE") != nullptr;
    config.bootstrap_timeout_ms = 20000;
    if (const char* policy = std::getenv("CL_PAR_PLACEMENT_POLICY"))
        config.placement_policy = static_cast<cl_placement_policy_t>(std::strtoul(policy, nullptr, 10));
    CHECK(cl_runtime_create(&config, &runtime) == CL_OK);
    std::cout << "region ready host=" << host << std::endl;
    cl_pthread_barrier_t barrier{};
    CHECK(cl_pthread_barrier_init(runtime, &barrier, 1) == CL_OK);
    cl_pthread_t thread{};
    std::uint64_t seed = 7;
    CHECK(cl_pthread_create(runtime, &thread, Leaf, &seed, sizeof(seed)) == CL_FAILED_PRECONDITION);
    std::vector<cl_pthread_function_t> manifest{
        {"example.worker", Worker, 1, sizeof(Argument), 0x574f524b45523031ULL},
        {"example.leaf", Leaf, 1, sizeof(seed), 0x4c45414630303031ULL}};
    if (host == 15 && mode == "missing") manifest.pop_back();
    if (host == 15 && mode == "schema") manifest[0].schema_id++;
    if (host == 15 && mode == "abi") manifest[0].abi_version++;
    if (host % 2) std::reverse(manifest.begin(), manifest.end());
    // Hold host 15 at a separate bootstrap barrier until every other host has
    // observed a timeout. This avoids relying on process scheduling or sleeps.
    if (mode == "timeout") {
        if (host != 15)
            CHECK(cl_pthread_register_functions(runtime, manifest.data(), manifest.size(), 10) == CL_UNAVAILABLE);
        Barrier(barrier);
    }
    auto registered = cl_pthread_register_functions(runtime, manifest.data(), manifest.size(), 20000);
    const bool negative = mode == "missing" || mode == "schema" || mode == "abi";
    if (negative) {
        CHECK(registered == CL_FAILED_PRECONDITION);
        Argument arg{};
        CHECK(cl_pthread_create(runtime, &thread, Worker, &arg, sizeof(arg)) == CL_FAILED_PRECONDITION);
        CHECK(executed == 0);
        Barrier(barrier);
    } else {
        CHECK(registered == CL_OK);
        CHECK(cl_pthread_register_functions(runtime, manifest.data(), manifest.size(), 20000) == CL_OK);
        CHECK(cl_pthread_create(runtime, &thread, Unregistered, nullptr, 0) == CL_NOT_FOUND);
        CHECK(cl_pthread_create(runtime, &thread, Leaf, nullptr, 0) == CL_INVALID_ARGUMENT);
        auto conflict = manifest;
        conflict[0].schema_id++;
        CHECK(cl_pthread_register_functions(runtime, conflict.data(), conflict.size(), 100) == CL_FAILED_PRECONDITION);
        Barrier(barrier);
        constexpr unsigned joined = 32, detached = 16, total = joined + detached;
        unsigned remote_results = 0;
        for (unsigned creator = 0; creator < 16; ++creator) {
            cl_gptr_t output{};
            if (host == creator) {
                CHECK(cl_mem_alloc(runtime, total * sizeof(std::uint64_t), 64, &output) == CL_OK);
                std::uint64_t zero[total]{};
                CHECK(cl_mem_write(runtime, output, 0, zero, sizeof(zero), 10000) == CL_OK);
                std::vector<cl_pthread_t> threads(joined);
                for (unsigned i = 0; i < joined; ++i) {
                    Argument args{output, i, host, i % 4 == 0};
                    CHECK(cl_pthread_create(runtime, &threads[i], Worker, &args, sizeof(args)) == CL_OK);
                }
                for (unsigned i = 0; i < joined; ++i) {
                    void* result = nullptr;
                    CHECK(cl_pthread_join(runtime, threads[i], &result) == CL_OK);
                    auto token = reinterpret_cast<std::uintptr_t>(result);
                    CHECK((token & 0xffffffffULL) == i + 1);
                    CHECK((token >> 32) >= 1 && (token >> 32) <= 16);
                    remote_results += (token >> 32) != host + 1;
                    CHECK(cl_pthread_join(runtime, threads[i], nullptr) == CL_NOT_FOUND);
                }
                for (unsigned i = joined; i < total; ++i) {
                    Argument args{output, i, host, 0};
                    CHECK(cl_pthread_create(runtime, &thread, Worker, &args, sizeof(args)) == CL_OK);
                    CHECK(cl_pthread_detach(runtime, thread) == CL_OK);
                    CHECK(cl_pthread_join(runtime, thread, nullptr) != CL_OK);
                }
                // Use a synchronization acquire on each poll; cl_mem_read alone
                // intentionally permits release-consistent cached snapshots.
                cl_pthread_mutex_t boundary{};
                CHECK(cl_pthread_mutex_init(runtime, &boundary) == CL_OK);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                for (;;) {
                    CHECK(cl_pthread_mutex_lock(&boundary) == CL_OK);
                    std::uint64_t values[total]{};
                    CHECK(cl_mem_read(runtime, output, 0, values, sizeof(values), 10000) == CL_OK);
                    CHECK(cl_pthread_mutex_unlock(&boundary) == CL_OK);
                    bool complete = true;
                    for (unsigned i = 0; i < total; ++i) complete &= (values[i] & 0xffffffffULL) == i + 1;
                    if (complete) {
                        for (unsigned i = joined; i < total; ++i) {
                            CHECK((values[i] >> 32) >= 1 && (values[i] >> 32) <= 16);
                            remote_results += (values[i] >> 32) != host + 1;
                        }
                        break;
                    }
                    CHECK(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                CHECK(cl_pthread_mutex_destroy(&boundary) == CL_OK);
                CHECK(cl_mem_free(runtime, output) == CL_OK);
            }
            Barrier(barrier);
        }
        CHECK(remote_results > 0);
        std::cout << "remote verification host=" << host << " remote_results=" << remote_results
                  << " executed=" << executed << " remote_executed=" << remote_executed << std::endl;
        Barrier(barrier);
    }
    CHECK(cl_pthread_barrier_destroy(&barrier) == CL_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        auto status = cl_runtime_finalize(runtime);
        if (status == CL_OK) break;
        CHECK(status == CL_FAILED_PRECONDITION && std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cl_runtime_destroy(runtime);
    std::cout << "PASS host=" << host << " hosts=16 mode=" << mode << " reclaimed=yes" << std::endl;
}
