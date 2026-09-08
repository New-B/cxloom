#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include "cxloom/loompar/runtime.h"

#define CHECK(x) do { if (!(x)) { std::cerr << "failed line " << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
using namespace cxloom;
std::atomic<bool> entered{false}, release_worker{false};
std::atomic<int> calls{0};
std::atomic<int> hook_phase{0};
void OrderedWorker(void*) { CHECK(hook_phase == 1); hook_phase = 2; }
std::thread::id execution_id;
void Block(void*) {
    execution_id = std::this_thread::get_id();
    entered = true;
    while (!release_worker) std::this_thread::yield();
    ++calls;
}
void Increment(void*) { ++calls; }
struct SelfContext {
    loompar::LoomParRuntime* runtime;
    GlobalThreadId id{};
    std::atomic<bool> ready{false};
    std::atomic<bool> rejected{false};
};
void SelfJoin(void* bytes) {
    SelfContext* context = nullptr;
    std::memcpy(&context, bytes, sizeof(context));
    while (!context->ready) std::this_thread::yield();
    context->rejected = !context->runtime->JoinThread(context->id).ok();
}
void Throw(void*) { throw std::runtime_error("test"); }
int main() {
    CxloomConfig config;
    config.shared_region_bytes = 192ULL << 20;
    loommem::LoomMemRuntime mem(config);
    CHECK(mem.Initialize().ok());
    loompar::LoomParRuntime par(config, &mem);
    CHECK(!par.CreateThread("block", {}, {}).ok());
    CHECK(par.Initialize().ok());
    CHECK(!par.RegisterFunction("null", nullptr).ok());
    CHECK(par.RegisterFunction("block", Block).ok());
    CHECK(!par.RegisterFunction("block", Increment).ok());
    CHECK(!par.CreateThread("missing", {}, {}).ok());
    auto id = par.CreateThread("block", {}, {});
    CHECK(id.ok());
    while (!entered) std::this_thread::yield();
    CHECK(execution_id != std::this_thread::get_id());
    CHECK(!par.JoinThread({1, id.value().local_tid}).ok());
    CHECK(!par.Finalize().ok());
    auto joined = std::async(std::launch::async, [&] { return par.JoinThread(id.value()); });
    CHECK(joined.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    release_worker = true;
    CHECK(joined.get().ok());
    CHECK(calls == 1);
    CHECK(par.thread_manager().size() == 0);
    CHECK(!par.JoinThread(id.value()).ok());
    CHECK(par.RegisterFunction("increment", Increment).ok());
    std::vector<std::future<void>> creators;
    for (int i = 0; i < 8; ++i) creators.push_back(std::async(std::launch::async, [&] {
        for (int j = 0; j < 40; ++j) {
            auto thread = par.CreateThread("increment", {}, {});
            CHECK(thread.ok()); CHECK(par.JoinThread(thread.value()).ok());
        }
    }));
    for (auto& creator : creators) creator.get();
    CHECK(calls == 321);
    CHECK(par.RegisterFunction("throw", Throw).ok());
    auto throwing = par.CreateThread("throw", {}, {});
    CHECK(throwing.ok()); CHECK(!par.JoinThread(throwing.value()).ok());
    CHECK(par.thread_manager().size() == 0);
    loompar::FunctionRegistry other;
    CHECK(other.Register("increment", Increment).value() == par.RegisterFunction("increment", Increment).value());
    CHECK(par.RegisterFunction("self", SelfJoin).ok());
    SelfContext context{&par};
    auto* pointer = &context;
    std::vector<std::byte> bytes(sizeof(pointer));
    std::memcpy(bytes.data(), &pointer, sizeof(pointer));
    auto self = par.CreateThread("self", bytes, {});
    CHECK(self.ok());
    context.id = self.value(); context.ready = true;
    while (!context.rejected) std::this_thread::yield();
    CHECK(par.JoinThread(self.value()).ok());
    loompar::ThreadManager states(0);
    auto fast = states.AllocateThread(1, 1, {});
    CHECK(fast.ok()); CHECK(states.MarkLaunching(fast.value()).ok());
    CHECK(states.MarkCompleted(fast.value(), 0).ok());
    CHECK(states.MarkRunning(fast.value()).ok());
    CHECK(states.Find(fast.value()).value().state == loompar::ThreadState::kCompleted);
    CHECK(states.Join(fast.value()).ok());
    auto ordered = states.AllocateThread(0, 1, {});
    CHECK(ordered.ok()); CHECK(states.MarkLaunching(ordered.value()).ok());
    CHECK(states.Launch(ordered.value(), OrderedWorker,
        [] { CHECK(hook_phase == 0); hook_phase = 1; return Status::Ok(); },
        [] { CHECK(hook_phase == 2); hook_phase = 3; return Status::Ok(); }).ok());
    CHECK(states.Join(ordered.value(), [] {
        CHECK(hook_phase == 3); hook_phase = 4; return Status::Ok();
    }).ok());
    CHECK(hook_phase == 4);
    auto failed_acquire = states.AllocateThread(0, 1, {});
    CHECK(failed_acquire.ok()); CHECK(states.MarkLaunching(failed_acquire.value()).ok());
    CHECK(states.Launch(failed_acquire.value(), OrderedWorker,
        [] { return Status::Internal("acquire failure"); },
        [] { CHECK(hook_phase == 4); hook_phase = 5; return Status::Ok(); }).ok());
    CHECK(!states.Join(failed_acquire.value(), [] {
        CHECK(hook_phase == 5); hook_phase = 6; return Status::Ok();
    }).ok());
    CHECK(hook_phase == 6 && states.size() == 0);
    CHECK(par.Finalize().ok()); CHECK(mem.Finalize().ok());
    std::cout << "local native execution, blocking join, concurrent creators and reclamation passed\n";
}
