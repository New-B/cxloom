#include "shared_region_fixture.h"
#include "cxloom/cxloom_mem.h"
#include "cxloom/loompar/runtime.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed " << __LINE__ << ": " << #x << std::endl; std::abort(); } } while (0)
using namespace cxloom;
namespace {
cl_runtime_t* runtime;
cl_pthread_mutex_t mutex;
cl_pthread_cond_t condition;
cl_pthread_barrier_t barrier;
std::atomic<int> waiting{0}, woke{0};
bool proceed = false;
void* Echo(void* bytes) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(*static_cast<int*>(bytes))); }
void* Nested(void* bytes) {
    int depth = *static_cast<int*>(bytes);
    if (depth) {
        --depth;
        cl_pthread_t child;
        CHECK(cl_pthread_create(runtime, &child, Nested, &depth, sizeof(depth)) == CL_OK);
        void* result = nullptr;
        CHECK(cl_pthread_join(runtime, child, &result) == CL_OK);
        CHECK(result == reinterpret_cast<void*>(0x42));
    }
    CHECK(cl_pthread_migration_safe_point(runtime) == CL_OK);
    CHECK(cl_pthread_migration_safe_point(runtime) == CL_OK);
    return reinterpret_cast<void*>(0x42);
}
void* BarrierWorker(void*) {
    for (int i = 0; i < 4; ++i) CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK);
    return nullptr;
}
void* Waiter(void*) {
    CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
    ++waiting;
    while (!proceed) CHECK(cl_pthread_cond_wait(&condition, &mutex) == CL_OK);
    ++woke;
    CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
    return nullptr;
}
void* Timed(void*) {
    CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
    CHECK(cl_pthread_cond_timedwait(&condition, &mutex, 20) == CL_UNAVAILABLE);
    CHECK(cl_pthread_mutex_trylock(&mutex) == CL_UNAVAILABLE);
    CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
    return nullptr;
}
void* Fail(void*) { throw 1; }
}
int main() {
    CxloomConfig cfg; cfg.shared_region_bytes = 192ULL << 20;
    SharedRegionFixture region(cfg);
    cl_config_t c{}; c.host_count = 1; c.shared_region_bytes = cfg.shared_region_bytes;
    c.coherence_granule_bytes = 4096; c.shared_region_path = cfg.shared_region_path.c_str();
    c.bootstrap_owner = 1; c.create_region_file = 1;
    CHECK(cl_runtime_create(&c, &runtime) == CL_OK);
    constexpr int count = 24;
    cl_pthread_t threads[count];
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < count; ++i) { int value = i + 1; CHECK(cl_pthread_create(runtime, &threads[i], Echo, &value, sizeof(value)) == CL_OK); }
        for (int i = 0; i < count; ++i) { void* result = nullptr; CHECK(cl_pthread_join(runtime, threads[i], &result) == CL_OK); CHECK(result == reinterpret_cast<void*>(i + 1)); }
    }
    int depth = 12;
    CHECK(cl_pthread_create(runtime, &threads[0], Nested, &depth, sizeof(depth)) == CL_OK);
    void* result = nullptr;
    CHECK(cl_pthread_join(runtime, threads[0], &result) == CL_OK && result == reinterpret_cast<void*>(0x42));
    CHECK(cl_pthread_create(runtime, &threads[0], Fail, nullptr, 0) == CL_OK);
    result = reinterpret_cast<void*>(0x99);
    CHECK(cl_pthread_join(runtime, threads[0], &result) == CL_INTERNAL && result == reinterpret_cast<void*>(0x99));
    CHECK(cl_pthread_barrier_init(runtime, &barrier, count) == CL_OK);
    for (auto& t : threads) CHECK(cl_pthread_create(runtime, &t, BarrierWorker, nullptr, 0) == CL_OK);
    for (auto t : threads) CHECK(cl_pthread_join(runtime, t, nullptr) == CL_OK);
    CHECK(cl_pthread_barrier_destroy(&barrier) == CL_OK);
    CHECK(cl_pthread_mutex_init(runtime, &mutex) == CL_OK);
    CHECK(cl_pthread_cond_init(runtime, &condition) == CL_OK);
    for (auto& t : threads) CHECK(cl_pthread_create(runtime, &t, Waiter, nullptr, 0) == CL_OK);
    while (waiting != count) std::this_thread::yield();
    CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
    CHECK(cl_pthread_cond_destroy(&condition) == CL_FAILED_PRECONDITION);
    proceed = true;
    CHECK(cl_pthread_cond_signal(&condition) == CL_OK);
    CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
    while (woke == 0) std::this_thread::yield();
    CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
    CHECK(woke == 1);
    CHECK(cl_pthread_cond_broadcast(&condition) == CL_OK);
    CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
    for (auto t : threads) CHECK(cl_pthread_join(runtime, t, nullptr) == CL_OK);
    CHECK(woke == count);
    for (auto& t : threads) CHECK(cl_pthread_create(runtime, &t, Timed, nullptr, 0) == CL_OK);
    for (auto t : threads) CHECK(cl_pthread_join(runtime, t, nullptr) == CL_OK);
    CHECK(cl_pthread_cond_destroy(&condition) == CL_OK);
    CHECK(cl_pthread_mutex_destroy(&mutex) == CL_OK);
    for (int i = 0; i < 100; ++i) {
        CHECK(cl_pthread_create(runtime, &threads[0], Echo, &i, sizeof(i)) == CL_OK);
        CHECK(cl_pthread_detach(runtime, threads[0]) == CL_OK);
    }
    const auto drained = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto status = cl_runtime_finalize(runtime);
        if (status == CL_OK) break;
        CHECK(status == CL_FAILED_PRECONDITION && std::chrono::steady_clock::now() < drained);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cl_runtime_destroy(runtime);

    // Detach retains active execution, releases capacity without join, and
    // handles completion-before-detach. No native handle or fiber cycle leaks.
    loommem::LoomMemRuntime mem(cfg); CHECK(mem.Initialize().ok());
    cfg.max_running_threads_per_host = 1;
    loompar::LoomParRuntime par(cfg, &mem); CHECK(par.Initialize().ok());
    std::atomic<bool> release{false}, entered{false};
    CHECK(par.RegisterFunction("hold", [&](void*) {
        entered = true;
        while (!release) {
            auto state = std::make_shared<loompar::Fiber::WaitState>();
            state->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
            loompar::Fiber::Current()->Park(state);
        }
    }).ok());
    auto detached = par.CreateThread("hold", {}, {}); CHECK(detached.ok());
    CHECK(par.DetachThread(detached.value()).ok());
    CHECK(!par.DetachThread(detached.value()).ok());
    CHECK(!par.JoinThread(detached.value()).ok());
    CHECK(!par.Finalize().ok());
    release = true;
    while (par.thread_manager().size()) std::this_thread::yield();
    for (int i = 0; i < 100; ++i) {
        auto next = par.CreateThread("hold", {}, {}); CHECK(next.ok());
        while (par.thread_manager().Find(next.value()).value().state != loompar::ThreadState::kCompleted) std::this_thread::yield();
        CHECK(par.DetachThread(next.value()).ok());
    }
    CHECK(par.Finalize().ok());
    // Alternating fibers on one native thread must not drain each other's writes.
    auto object = mem.AllocateShared(64, 64); CHECK(object.ok());
    loompar::Fiber first(64 * 1024, [&] {
        auto write = mem.AcquireWriteBuffer(object.value(), 1000); CHECK(write.ok());
        CHECK(mem.StageWriteBuffer(&write.value()).ok());
        loompar::Fiber::Yield();
        CHECK(mem.staged_write_count() == 1);
        CHECK(mem.SynchronizeRelease().ok());
    });
    loompar::Fiber second(64 * 1024, [&] { CHECK(mem.SynchronizeRelease().ok()); CHECK(mem.staged_write_count() == 1); });
    CHECK(first.Resume().ok()); CHECK(second.Resume().ok()); CHECK(first.Resume().ok());
    CHECK(mem.staged_write_count() == 0);
    {
        loompar::ThreadManager manager(0);
        std::atomic<bool> held{false}, unlock{false};
        std::atomic<int> acquired{0};
        std::vector<GlobalThreadId> ids;
        auto launch = [&](loompar::ThreadFunction fn) {
            auto id = manager.AllocateThread(0, 1, {}); CHECK(id.ok());
            CHECK(manager.MarkLaunching(id.value()).ok());
            CHECK(manager.Launch(id.value(), std::move(fn)).ok());
            ids.push_back(id.value());
        };
        launch([&](void*) {
            auto write = mem.AcquireWriteBuffer(object.value(), 1000); CHECK(write.ok());
            held = true;
            while (!unlock) {
                auto state = std::make_shared<loompar::Fiber::WaitState>();
                state->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
                loompar::Fiber::Current()->Park(state);
            }
            CHECK(mem.ReleaseWriteBuffer(write.value()).ok());
        });
        while (!held) std::this_thread::yield();
        for (int i = 0; i < 24; ++i) launch([&](void*) {
            auto write = mem.AcquireWriteBuffer(object.value(), 2000); CHECK(write.ok());
            ++acquired;
            CHECK(mem.ReleaseWriteBuffer(write.value()).ok());
        });
        launch([&](void*) { unlock = true; });
        for (auto id : ids) CHECK(manager.Join(id).ok());
        CHECK(acquired == 24);
    }
    CHECK(mem.FreeShared(object.value()).ok()); CHECK(mem.Finalize().ok());
    std::cout << "results, detach, cooperative joins/barriers/conditions and staged isolation passed\n";
}
