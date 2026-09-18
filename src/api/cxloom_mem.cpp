#include "cxloom/cxloom_mem.h"

#include <new>
#include <memory>
#include <string>
#include <dlfcn.h>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <unordered_map>

#include "cxloom/common/config.h"
#include "cxloom/common/execution_context.h"
#include "cxloom/common/tracing.h"
#include "cxloom/loommem/runtime.h"
#include "cxloom/loompar/runtime.h"

struct cl_runtime {
    explicit cl_runtime(cxloom::CxloomConfig value) : config(value), loommem(std::move(value)) {}

    cxloom::CxloomConfig config;
    cxloom::loommem::LoomMemRuntime loommem;
    std::unique_ptr<cxloom::loompar::LoomParRuntime> loompar;
    std::mutex par_mutex;
};

constexpr std::uint64_t kMutexMagic = 0x434c4d5554455831ULL;
constexpr std::uint64_t kConditionMagic = 0x434c434f4e443031ULL;

struct ClMutex {
    cl_runtime_t* runtime{nullptr};
    cxloom::GlobalPointer object{};
    bool distributed{false};
    bool owns_object{true};
    cxloom::loommem::ObjectReference reference;
    std::mutex guard;
    cxloom::loompar::Condition available;
    bool locked{false};
    std::uint64_t owner{0};
    std::size_t waiters{0};
    std::unique_ptr<cxloom::loommem::WriteBuffer> lease;
};
struct ClCond {
    cl_runtime_t* runtime{nullptr};
    cxloom::GlobalPointer object{};
    bool distributed{false};
    bool owns_object{true};
    cxloom::loommem::ObjectReference reference;
    std::mutex guard;
    cxloom::loompar::Condition value;
    std::size_t waiters{0};
};

namespace {

cl_status_t ToCStatus(const cxloom::Status& status) {
    return static_cast<cl_status_t>(status.code());
}

cxloom::CxloomConfig ToCppConfig(const cl_config_t& config) {
    cxloom::CxloomConfig result;
    result.local_host_id = config.local_host_id;
    result.host_count = config.host_count;
    result.shared_region_bytes = config.shared_region_bytes;
    result.coherence_granule_bytes = config.coherence_granule_bytes;
    result.queue_capacity_entries = config.queue_capacity_entries;
    if (config.shared_region_path != nullptr) {
        result.shared_region_path = config.shared_region_path;
    }
    result.bootstrap_owner = config.bootstrap_owner != 0;
    result.create_region_file = config.create_region_file != 0;
    result.placement_policy = static_cast<cxloom::PlacementPolicy>(config.placement_policy);
    if (config.bootstrap_timeout_ms != 0) {
        result.bootstrap_timeout_ms = config.bootstrap_timeout_ms;
    }
    if (config.replica_cache_capacity_entries != 0)
        result.replica_cache_capacity_entries = config.replica_cache_capacity_entries;
    if (config.replica_cache_capacity_bytes != 0)
        result.replica_cache_capacity_bytes = config.replica_cache_capacity_bytes;
    return result;
}

}  // namespace

extern "C" cl_status_t cl_runtime_create(const cl_config_t* config, cl_runtime_t** runtime) {
    if (config == nullptr || runtime == nullptr) {
        return CL_INVALID_ARGUMENT;
    }

    auto* value = new (std::nothrow) cl_runtime(ToCppConfig(*config));
    if (value == nullptr) {
        return CL_UNAVAILABLE;
    }
    const auto status = value->loommem.Initialize();
    if (!status.ok()) {
        delete value;
        return ToCStatus(status);
    }
    *runtime = value;
    return CL_OK;
}

extern "C" void cl_runtime_destroy(cl_runtime_t* runtime) {
    if (runtime != nullptr) {
        const auto status = cl_runtime_finalize(runtime);
        if (status != CL_OK) cxloom::Trace("cxloom", "runtime destroy forced after finalize failure");
        delete runtime;
    }
}

extern "C" cl_status_t cl_runtime_finalize(cl_runtime_t* runtime) {
    if (runtime == nullptr) return CL_INVALID_ARGUMENT;
    if (runtime->loompar != nullptr) {
        const auto status = runtime->loompar->Finalize();
        if (!status.ok()) return ToCStatus(status);
        runtime->loompar.reset();
    }
    return ToCStatus(runtime->loommem.Finalize());
}

namespace {
cl_status_t EnsurePar(cl_runtime_t* runtime) {
    if (runtime == nullptr) return CL_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(runtime->par_mutex);
    if (!runtime->loompar) {
        runtime->loompar = std::make_unique<cxloom::loompar::LoomParRuntime>(runtime->config, &runtime->loommem);
        const auto status = runtime->loompar->Initialize();
        if (!status.ok()) { runtime->loompar.reset(); return ToCStatus(status); }
    }
    return CL_OK;
}

std::string CallbackName(cl_pthread_start_routine function) {
    Dl_info info{};
    if (function != nullptr && dladdr(reinterpret_cast<const void*>(function), &info) && info.dli_sname)
        return info.dli_sname;
    return std::string("__cxloom_callback_") + std::to_string(reinterpret_cast<std::uintptr_t>(function));
}
}

extern "C" cl_status_t cl_pthread_register_functions(cl_runtime_t* runtime,
                                                        const cl_pthread_function_t* functions,
                                                        size_t count, uint64_t timeout_ms) {
    if (!runtime || (count && !functions) || count > cxloom::loompar::kMaxClusterFunctions ||
        !timeout_ms || timeout_ms > UINT32_MAX) return CL_INVALID_ARGUMENT;
    std::vector<cxloom::loompar::ClusterFunction> manifest;
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = functions[i];
        if (!entry.name || !entry.start_routine) return CL_INVALID_ARGUMENT;
        const auto name_bytes = strnlen(entry.name, cxloom::loompar::kMaxClusterFunctionName + 1);
        if (!name_bytes || name_bytes > cxloom::loompar::kMaxClusterFunctionName) return CL_INVALID_ARGUMENT;
        const auto callback = entry.start_routine;
        manifest.push_back({std::string(entry.name, name_bytes), entry.abi_version, entry.argument_bytes,
                            entry.schema_id, reinterpret_cast<std::uintptr_t>(callback),
                            [callback](void* bytes) {
                                cxloom::loompar::ThreadManager::SetCurrentResult(static_cast<std::uint64_t>(
                                    reinterpret_cast<std::uintptr_t>(callback(bytes))));
                            }});
    }
    auto status = EnsurePar(runtime);
    if (status != CL_OK) return status;
    return ToCStatus(runtime->loompar->RegisterClusterFunctions(std::move(manifest), timeout_ms));
}

extern "C" cl_status_t cl_pthread_create(cl_runtime_t* runtime, cl_pthread_t* thread,
    cl_pthread_start_routine start_routine, const void* arg, size_t arg_bytes) {
    return cl_pthread_create_with_working_set(runtime, thread, start_routine, arg, arg_bytes, nullptr, 0);
}
extern "C" cl_status_t cl_pthread_create_with_working_set(cl_runtime_t* runtime, cl_pthread_t* thread,
    cl_pthread_start_routine start_routine, const void* arg, size_t arg_bytes,
    const cl_working_set_entry_t* working_set, size_t count) {
    if (runtime == nullptr || thread == nullptr || start_routine == nullptr || (arg_bytes && arg == nullptr))
        return CL_INVALID_ARGUMENT;
    if (arg_bytes > 104) return CL_INVALID_ARGUMENT;
    if (count > 256 || (count && !working_set)) return CL_INVALID_ARGUMENT;
    cxloom::ThreadPlacementHint hint;
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = working_set[i];
        hint.working_set.push_back({{entry.object.region_id, entry.object.offset}, entry.offset, entry.bytes,
            static_cast<cxloom::MemoryAccess>(entry.access), entry.weight});
    }
    auto status = EnsurePar(runtime);
    if (status != CL_OK) return status;
    std::vector<std::byte> bytes(arg_bytes);
    if (arg_bytes) std::memcpy(bytes.data(), arg, arg_bytes);
    cxloom::Result<cxloom::GlobalThreadId> created = cxloom::Status::NotFound("unregistered callback");
    if (runtime->config.host_count > 1 || runtime->loompar->has_cluster_manifest()) {
        created = runtime->loompar->CreateRegisteredThread(reinterpret_cast<std::uintptr_t>(start_routine), std::move(bytes), hint);
    } else {
        // Backwards-compatible single-host calls need no manifest or symbol export.
        const auto name = CallbackName(start_routine);
        auto registration = runtime->loompar->RegisterFunction(name, [start_routine](void* data) {
            cxloom::loompar::ThreadManager::SetCurrentResult(static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(start_routine(data))));
        });
        if (!registration.ok()) return ToCStatus(registration.status());
        created = runtime->loompar->CreateThread(name, std::move(bytes), hint);
    }
    if (!created.ok()) return ToCStatus(created.status());
    thread->home_host = created.value().home_host;
    thread->local_tid = created.value().local_tid;
    return CL_OK;
}

extern "C" cl_status_t cl_pthread_join(cl_runtime_t* runtime, cl_pthread_t thread, void** retval) {
    if (runtime == nullptr || runtime->loompar == nullptr) return CL_INVALID_ARGUMENT;
    std::uint64_t result = 0;
    auto status = runtime->loompar->JoinThread({thread.home_host, thread.local_tid}, &result);
    if (status.ok() && retval) *retval = reinterpret_cast<void*>(static_cast<std::uintptr_t>(result));
    return ToCStatus(status);
}

extern "C" cl_status_t cl_pthread_detach(cl_runtime_t* runtime, cl_pthread_t thread) {
    if (runtime == nullptr || runtime->loompar == nullptr) return CL_INVALID_ARGUMENT;
    return ToCStatus(runtime->loompar->DetachThread({thread.home_host, thread.local_tid}));
}

extern "C" cl_status_t cl_pthread_migration_safe_point(cl_runtime_t* runtime) {
    if (runtime == nullptr || runtime->loompar == nullptr) return CL_INVALID_ARGUMENT;
    return ToCStatus(cxloom::loompar::ThreadManager::CurrentSafePoint());
}

extern "C" cl_status_t cl_pthread_mutex_init(cl_runtime_t* runtime, cl_pthread_mutex_t* mutex) {
    if (!runtime || !mutex) return CL_INVALID_ARGUMENT;
    auto* impl = new (std::nothrow) ClMutex();
    if (!impl) return CL_UNAVAILABLE;
    impl->runtime = runtime;
    if (runtime->config.host_count > 1) {
        auto status = EnsurePar(runtime);
        if (status != CL_OK) { delete impl; return status; }
        auto object = runtime->loommem.AllocateShared(64, 64);
        if (!object.ok()) { delete impl; return ToCStatus(object.status()); }
        impl->object = object.value();
        impl->distributed = true;
    }
    if (impl->distributed) {
        auto status = cl_mem_write(runtime, {impl->object.region_id, impl->object.offset}, 0, &kMutexMagic, sizeof(kMutexMagic), 10000);
        if (status != CL_OK) { runtime->loommem.FreeShared(impl->object); delete impl; return status; }
    }
    mutex->impl = impl;
    return CL_OK;
}
namespace {
void CooperativePause() {
    if (auto* fiber = cxloom::loompar::Fiber::Current()) {
        auto state = std::make_shared<cxloom::loompar::Fiber::WaitState>();
        state->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        fiber->Park(std::move(state));
    } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
cl_status_t LockMutex(cl_pthread_mutex_t* mutex, bool attempt) {
    if (!mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    std::unique_lock<std::mutex> lock(impl->guard);
    const auto owner = cxloom::CurrentExecutionContextId();
    if (impl->locked && impl->owner == owner) return attempt ? CL_UNAVAILABLE : CL_FAILED_PRECONDITION;
    if (attempt && impl->locked) return CL_UNAVAILABLE;
    ++impl->waiters;
    impl->available.wait(lock, [&] { return !impl->locked; });
    --impl->waiters;
    impl->locked = true;
    impl->owner = owner;
    lock.unlock();
    cl_status_t status = CL_OK;
    if (impl->distributed) {
        for (;;) {
            auto lease = impl->runtime->loommem.AcquireWriteBuffer(impl->object, attempt ? 1 : 10000);
            if (lease.ok()) {
                impl->lease = std::make_unique<cxloom::loommem::WriteBuffer>(std::move(lease.value()));
                break;
            }
            status = ToCStatus(lease.status());
            if (attempt || status != CL_UNAVAILABLE) break;
            CooperativePause();
        }
        if (impl->lease) status = CL_OK;
    }
    if (status == CL_OK) status = ToCStatus(impl->runtime->loommem.SynchronizeAcquire());
    if (status != CL_OK) {
        if (impl->lease) { impl->runtime->loommem.ReleaseWriteBuffer(*impl->lease); impl->lease.reset(); }
        lock.lock(); impl->locked = false; impl->owner = 0; impl->available.notify_one();
    }
    return status;
}
}
extern "C" cl_status_t cl_pthread_mutex_lock(cl_pthread_mutex_t* mutex) { return LockMutex(mutex, false); }
extern "C" cl_status_t cl_pthread_mutex_trylock(cl_pthread_mutex_t* mutex) { return LockMutex(mutex, true); }
extern "C" cl_status_t cl_pthread_mutex_unlock(cl_pthread_mutex_t* mutex) {
    if (!mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    std::lock_guard<std::mutex> lock(impl->guard);
    if (!impl->locked || impl->owner != cxloom::CurrentExecutionContextId()) return CL_FAILED_PRECONDITION;
    auto status = impl->runtime->loommem.SynchronizeRelease();
    if (!status.ok()) return ToCStatus(status);
    if (impl->distributed) {
        status = impl->runtime->loommem.ReleaseWriteBuffer(*impl->lease);
        if (!status.ok()) return ToCStatus(status);
        impl->lease.reset();
    }
    impl->locked = false; impl->owner = 0;
    impl->available.notify_one();
    return CL_OK;
}
extern "C" cl_status_t cl_pthread_mutex_destroy(cl_pthread_mutex_t* mutex) {
    if (!mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    {
        std::lock_guard<std::mutex> lock(impl->guard);
        if (impl->locked || impl->waiters) return CL_FAILED_PRECONDITION;
        if (impl->distributed && impl->owns_object) {
            auto status = impl->runtime->loommem.FreeShared(impl->object);
            if (!status.ok()) return ToCStatus(status);
        }
    }
    delete impl; mutex->impl = nullptr; return CL_OK;
}

extern "C" cl_status_t cl_pthread_cond_init(cl_runtime_t* runtime, cl_pthread_cond_t* condition) {
    if (!runtime || !condition) return CL_INVALID_ARGUMENT;
    auto* impl = new (std::nothrow) ClCond();
    if (!impl) return CL_UNAVAILABLE;
    impl->runtime = runtime;
    if (runtime->config.host_count > 1) {
        auto status = EnsurePar(runtime);
        if (status != CL_OK) { delete impl; return status; }
        auto object = runtime->loommem.AllocateShared(64, 64);
        if (!object.ok()) { delete impl; return ToCStatus(object.status()); }
        impl->object = object.value();
        impl->distributed = true;
    }
    if (impl->distributed) {
        const std::uint64_t zero[3] = {kConditionMagic, 0, 0};
        auto status = cl_mem_write(runtime, {impl->object.region_id, impl->object.offset}, 0, zero, sizeof(zero), 10000);
        if (status != CL_OK) { runtime->loommem.FreeShared(impl->object); delete impl; return status; }
    }
    condition->impl = impl; return CL_OK;
}
namespace {
// A shared condition reserves one bit per active waiter. Signal selects one
// registered waiter; broadcast selects the complete registered cohort.
struct SharedCondition { std::uint64_t magic{kConditionMagic}; std::uint64_t waiting{0}; std::uint64_t ready{0}; };
cl_status_t ChangeCondition(ClCond* cond, const std::function<cl_status_t(SharedCondition&)>& change) {
    for (;;) {
        auto buffer = cond->runtime->loommem.AcquireWriteBuffer(cond->object, 10000);
        if (!buffer.ok()) {
            if (buffer.status().code() != cxloom::StatusCode::kUnavailable) return ToCStatus(buffer.status());
            CooperativePause(); continue;
        }
        auto* state = static_cast<SharedCondition*>(buffer.value().data());
        auto result = change(*state);
        auto released = cond->runtime->loommem.ReleaseWriteBuffer(buffer.value());
        return released.ok() ? result : ToCStatus(released);
    }
}
cl_status_t WaitCondition(cl_pthread_cond_t* condition, cl_pthread_mutex_t* mutex, uint64_t timeout_ms) {
    if (!condition || !condition->impl || !mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    auto* cond = static_cast<ClCond*>(condition->impl);
    if (impl->runtime != cond->runtime) return CL_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> guard(impl->guard);
        if (!impl->locked || impl->owner != cxloom::CurrentExecutionContextId()) return CL_FAILED_PRECONDITION;
    }
    const auto deadline = timeout_ms ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
                                    : std::chrono::steady_clock::time_point::max();
    std::unique_lock<std::mutex> lock(cond->guard);
    ++cond->waiters;
    cl_status_t status = CL_OK;
    std::uint64_t bit = 0;
    if (cond->distributed) {
        lock.unlock();
        status = ChangeCondition(cond, [&](SharedCondition& state) {
            auto free = ~state.waiting;
            if (!free) return CL_UNAVAILABLE;
            bit = free & (~free + 1);
            state.waiting |= bit;
            state.ready &= ~bit;
            return CL_OK;
        });
        lock.lock();
        if (status != CL_OK) { --cond->waiters; return status; }
    }
    status = cl_pthread_mutex_unlock(mutex);
    if (status != CL_OK) {
        if (bit) { lock.unlock(); ChangeCondition(cond, [&](SharedCondition& state) {
            state.waiting &= ~bit; state.ready &= ~bit; return CL_OK;
        }); lock.lock(); }
        --cond->waiters; return status;
    }
    if (!cond->distributed) {
        status = cond->value.wait_until(lock, deadline) == std::cv_status::timeout ? CL_UNAVAILABLE : CL_OK;
    } else {
        lock.unlock();
        for (;;) {
            auto acquired = cond->runtime->loommem.SynchronizeAcquire();
            if (!acquired.ok()) { status = ToCStatus(acquired); break; }
            SharedCondition state;
            status = cl_mem_read(cond->runtime, {cond->object.region_id, cond->object.offset}, 0, &state, sizeof(state), 1);
            if (status == CL_OK && (state.ready & bit)) break;
            if (status != CL_OK && status != CL_UNAVAILABLE) break;
            if (std::chrono::steady_clock::now() >= deadline) { status = CL_UNAVAILABLE; break; }
            CooperativePause();
        }
        auto cleanup = ChangeCondition(cond, [&](SharedCondition& state) {
            state.waiting &= ~bit; state.ready &= ~bit; return CL_OK;
        });
        if (cleanup != CL_OK) status = cleanup;
        lock.lock();
    }
    // Keep the waiter registered until it has reacquired the application mutex.
    lock.unlock();
    auto reacquired = cl_pthread_mutex_lock(mutex);
    lock.lock(); --cond->waiters;
    return reacquired == CL_OK ? status : reacquired;
}
cl_status_t NotifyCondition(cl_pthread_cond_t* condition, bool all) {
    if (!condition || !condition->impl) return CL_INVALID_ARGUMENT;
    auto* cond = static_cast<ClCond*>(condition->impl);
    if (cond->distributed) return ChangeCondition(cond, [&](SharedCondition& state) {
        auto pending = state.waiting & ~state.ready;
        state.ready |= all ? pending : pending & (~pending + 1);
        return CL_OK;
    });
    std::lock_guard<std::mutex> lock(cond->guard);
    if (all) cond->value.notify_all(); else cond->value.notify_one();
    return CL_OK;
}
}
extern "C" cl_status_t cl_pthread_cond_wait(cl_pthread_cond_t* condition, cl_pthread_mutex_t* mutex) {
    return WaitCondition(condition, mutex, 0);
}
extern "C" cl_status_t cl_pthread_cond_timedwait(cl_pthread_cond_t* condition, cl_pthread_mutex_t* mutex, uint64_t timeout_ms) {
    if (!timeout_ms) return CL_INVALID_ARGUMENT;
    return WaitCondition(condition, mutex, timeout_ms);
}
extern "C" cl_status_t cl_pthread_cond_signal(cl_pthread_cond_t* condition) { return NotifyCondition(condition, false); }
extern "C" cl_status_t cl_pthread_cond_broadcast(cl_pthread_cond_t* condition) { return NotifyCondition(condition, true); }
extern "C" cl_status_t cl_pthread_cond_destroy(cl_pthread_cond_t* condition) {
    if (!condition || !condition->impl) return CL_INVALID_ARGUMENT;
    auto* cond = static_cast<ClCond*>(condition->impl);
    {
        std::lock_guard<std::mutex> lock(cond->guard);
        if (cond->waiters) return CL_FAILED_PRECONDITION;
        if (cond->distributed && cond->owns_object) {
            auto status = cond->runtime->loommem.FreeShared(cond->object);
            if (!status.ok()) return ToCStatus(status);
        }
    }
    delete cond; condition->impl = nullptr; return CL_OK;
}

namespace {
template<class Impl, class Handle>
cl_status_t ExportSync(Handle* handle, cl_gptr_t* object) {
    if (!handle || !handle->impl || !object) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<Impl*>(handle->impl);
    if (!impl->distributed) return CL_FAILED_PRECONDITION;
    *object = {impl->object.region_id, impl->object.offset};
    return CL_OK;
}
template<class Impl, class Handle>
cl_status_t AttachSync(cl_runtime_t* runtime, Handle* handle, cl_gptr_t object, std::uint64_t magic) {
    if (!runtime || !handle || runtime->config.host_count < 2) return CL_INVALID_ARGUMENT;
    auto status = EnsurePar(runtime);
    if (status != CL_OK) return status;
    auto reference = runtime->loommem.AcquireObjectReference({object.region_id, object.offset});
    if (!reference.ok()) return ToCStatus(reference.status());
    auto read = runtime->loommem.AcquireReadSnapshot({object.region_id, object.offset}, 10000);
    if (!read.ok()) return ToCStatus(read.status());
    std::uint64_t observed = 0;
    if (read.value().bytes() != 64) return CL_INVALID_ARGUMENT;
    std::memcpy(&observed, read.value().data(), sizeof(observed));
    if (observed != magic) return CL_INVALID_ARGUMENT;
    auto* impl = new (std::nothrow) Impl();
    if (!impl) return CL_UNAVAILABLE;
    impl->runtime = runtime; impl->object = {object.region_id, object.offset};
    impl->distributed = true; impl->owns_object = false;
    impl->reference = std::move(reference.value());
    handle->impl = impl;
    return CL_OK;
}
}
extern "C" cl_status_t cl_pthread_mutex_export(cl_pthread_mutex_t* mutex, cl_gptr_t* object) {
    return ExportSync<ClMutex>(mutex, object);
}
extern "C" cl_status_t cl_pthread_mutex_attach(cl_runtime_t* runtime, cl_pthread_mutex_t* mutex, cl_gptr_t object) {
    return AttachSync<ClMutex>(runtime, mutex, object, kMutexMagic);
}
extern "C" cl_status_t cl_pthread_cond_export(cl_pthread_cond_t* condition, cl_gptr_t* object) {
    return ExportSync<ClCond>(condition, object);
}
extern "C" cl_status_t cl_pthread_cond_attach(cl_runtime_t* runtime, cl_pthread_cond_t* condition, cl_gptr_t object) {
    return AttachSync<ClCond>(runtime, condition, object, kConditionMagic);
}

extern "C" cl_status_t cl_pthread_barrier_init(cl_runtime_t* runtime, cl_pthread_barrier_t* barrier,
                                                size_t local_participants) {
    if (runtime == nullptr || barrier == nullptr || local_participants == 0) return CL_INVALID_ARGUMENT;
    auto status = EnsurePar(runtime);
    if (status != CL_OK) return status;
    static std::atomic<std::uint64_t> next_id{1};
    barrier->runtime = runtime;
    barrier->barrier_id = next_id.fetch_add(1, std::memory_order_relaxed);
    barrier->local_participants = local_participants;
    return CL_OK;
}

extern "C" cl_status_t cl_pthread_barrier_wait(cl_pthread_barrier_t* barrier) {
    if (barrier == nullptr || barrier->runtime == nullptr || barrier->runtime->loompar == nullptr)
        return CL_INVALID_ARGUMENT;
    return ToCStatus(barrier->runtime->loompar->Barrier(barrier->barrier_id, barrier->local_participants));
}

extern "C" cl_status_t cl_pthread_barrier_destroy(cl_pthread_barrier_t* barrier) {
    if (barrier == nullptr || barrier->runtime == nullptr) return CL_INVALID_ARGUMENT;
    barrier->runtime = nullptr;
    barrier->barrier_id = 0;
    barrier->local_participants = 0;
    return CL_OK;
}

extern "C" cl_status_t cl_mem_alloc(cl_runtime_t* runtime, size_t bytes, size_t alignment,
                                     cl_gptr_t* out_gptr) {
    if (runtime == nullptr || out_gptr == nullptr) {
        return CL_INVALID_ARGUMENT;
    }
    const auto result = runtime->loommem.AllocateShared(bytes, alignment);
    if (!result.ok()) {
        return ToCStatus(result.status());
    }
    *out_gptr = {result.value().region_id, result.value().offset};
    return CL_OK;
}

extern "C" cl_status_t cl_mem_free(cl_runtime_t* runtime, cl_gptr_t gptr) {
    if (runtime == nullptr) {
        return CL_INVALID_ARGUMENT;
    }
    return ToCStatus(runtime->loommem.FreeShared({gptr.region_id, gptr.offset}));
}

extern "C" cl_status_t cl_mem_read(cl_runtime_t* runtime, cl_gptr_t gptr, size_t offset,
                                     void* out_bytes, size_t bytes, uint64_t timeout_ms) {
    if (!runtime || !out_bytes || bytes == 0 || timeout_ms == 0) return CL_INVALID_ARGUMENT;
    auto result = runtime->loommem.AcquireReadRange({gptr.region_id, gptr.offset}, offset, bytes,
                                                     timeout_ms);
    if (!result.ok()) return ToCStatus(result.status());
    std::memcpy(out_bytes, result.value().data(), bytes);
    return CL_OK;
}

extern "C" cl_status_t cl_mem_write(cl_runtime_t* runtime, cl_gptr_t gptr, size_t offset,
                                      const void* bytes, size_t byte_count, uint64_t timeout_ms) {
    if (!runtime || !bytes || byte_count == 0 || timeout_ms == 0) return CL_INVALID_ARGUMENT;
    auto result = runtime->loommem.AcquireWriteRange({gptr.region_id, gptr.offset}, offset, byte_count,
                                                      timeout_ms);
    if (!result.ok()) return ToCStatus(result.status());
    std::memcpy(result.value().data(), bytes, byte_count);
    return ToCStatus(runtime->loommem.ReleaseWriteBuffer(result.value()));
}

extern "C" cl_status_t cl_mem_resolve_local(cl_runtime_t* runtime, cl_gptr_t gptr, void** out_address) {
    if (runtime == nullptr || out_address == nullptr) {
        return CL_INVALID_ARGUMENT;
    }
    const auto result = runtime->loommem.ResolveLocal({gptr.region_id, gptr.offset});
    if (!result.ok()) {
        return ToCStatus(result.status());
    }
    *out_address = result.value();
    return CL_OK;
}
