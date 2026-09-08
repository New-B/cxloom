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
#include "cxloom/common/tracing.h"
#include "cxloom/loommem/runtime.h"
#include "cxloom/loompar/runtime.h"

struct cl_runtime {
    explicit cl_runtime(cxloom::CxloomConfig value) : config(value), loommem(std::move(value)) {}

    cxloom::CxloomConfig config;
    cxloom::loommem::LoomMemRuntime loommem;
    std::unique_ptr<cxloom::loompar::LoomParRuntime> loompar;
    std::mutex result_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<void*>> results;
};

struct ClMutex {
    cl_runtime_t* runtime{nullptr};
    cxloom::GlobalPointer object{};
    bool distributed{false};
    std::mutex value;
    std::unique_ptr<cxloom::loommem::WriteBuffer> lease;
};
struct ClCond {
    cl_runtime_t* runtime{nullptr};
    cxloom::GlobalPointer object{};
    bool distributed{false};
    std::condition_variable value;
    std::atomic<std::uint32_t> waiters{0};
};

static std::uint64_t ThreadKey(cl_pthread_t thread) {
    return (static_cast<std::uint64_t>(thread.home_host) << 48) ^ thread.local_tid;
}

namespace {

cl_status_t ToCStatus(const cxloom::Status& status) {
    return static_cast<cl_status_t>(status.code());
}

cxloom::CxloomConfig ToCppConfig(const cl_config_t& config) {
    cxloom::CxloomConfig result;
    result.local_host_id = config.local_host_id;
    result.host_count = config.host_count;
    result.shared_region_bytes = config.shared_region_bytes;
    result.per_host_extent_bytes = config.per_host_extent_bytes;
    result.coherence_granule_bytes = config.coherence_granule_bytes;
    result.default_coherence_granularity =
        config.default_coherence_granularity == CL_COHERENCE_FIXED_BLOCK
            ? cxloom::CoherenceGranularity::kFixedBlock
            : cxloom::CoherenceGranularity::kObject;
    result.queue_capacity_entries = config.queue_capacity_entries;
    if (config.shared_region_path != nullptr) {
        result.shared_region_path = config.shared_region_path;
    }
    result.bootstrap_owner = config.bootstrap_owner != 0;
    result.create_region_file = config.create_region_file != 0;
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

extern "C" cl_status_t cl_pthread_create(cl_runtime_t* runtime, cl_pthread_t* thread,
                                           cl_pthread_start_routine start_routine,
                                           const void* arg, size_t arg_bytes) {
    if (runtime == nullptr || thread == nullptr || start_routine == nullptr || (arg_bytes && arg == nullptr))
        return CL_INVALID_ARGUMENT;
    if (arg_bytes > 104) return CL_INVALID_ARGUMENT;
    auto status = EnsurePar(runtime);
    if (status != CL_OK) return status;
    const auto name = CallbackName(start_routine);
    auto result = std::make_shared<void*>(nullptr);
    auto registration = runtime->loompar->RegisterFunction(name, [start_routine, result](void* bytes) {
        *result = start_routine(bytes);
    });
    if (!registration.ok()) return ToCStatus(registration.status());
    std::vector<std::byte> bytes(arg_bytes);
    if (arg_bytes) std::memcpy(bytes.data(), arg, arg_bytes);
    auto created = runtime->loompar->CreateThread(name, std::move(bytes), {},
                                                   [result] { return static_cast<std::uint64_t>(
                                                       reinterpret_cast<std::uintptr_t>(*result)); });
    if (!created.ok()) return ToCStatus(created.status());
    thread->home_host = created.value().home_host;
    thread->local_tid = created.value().local_tid;
    { std::lock_guard<std::mutex> lock(runtime->result_mutex); runtime->results[ThreadKey(*thread)] = result; }
    return CL_OK;
}

extern "C" cl_status_t cl_pthread_join(cl_runtime_t* runtime, cl_pthread_t thread, void** retval) {
    if (runtime == nullptr || runtime->loompar == nullptr) return CL_INVALID_ARGUMENT;
    auto before = runtime->loompar->thread_manager().Find({thread.home_host, thread.local_tid});
    auto status = runtime->loompar->JoinThread({thread.home_host, thread.local_tid});
    if (retval) {
        std::lock_guard<std::mutex> lock(runtime->result_mutex);
        auto it = runtime->results.find(ThreadKey(thread));
        if (it != runtime->results.end()) { *retval = *it->second; runtime->results.erase(it); }
        else if (before.ok()) *retval = reinterpret_cast<void*>(static_cast<std::uintptr_t>(before.value().result_value));
    }
    return ToCStatus(status);
}

extern "C" cl_status_t cl_pthread_detach(cl_runtime_t* runtime, cl_pthread_t thread) {
    if (runtime == nullptr || runtime->loompar == nullptr) return CL_INVALID_ARGUMENT;
    auto status = runtime->loompar->thread_manager().Detach({thread.home_host, thread.local_tid});
    std::lock_guard<std::mutex> lock(runtime->result_mutex); runtime->results.erase(ThreadKey(thread));
    return ToCStatus(status);
}

extern "C" cl_status_t cl_pthread_migration_safe_point(cl_runtime_t* runtime) {
    if (runtime == nullptr || runtime->loompar == nullptr) return CL_INVALID_ARGUMENT;
    const auto gtid = cxloom::loompar::ThreadManager::CurrentGtid();
    return ToCStatus(runtime->loompar->thread_manager().MigrationSafePoint(gtid));
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
    mutex->impl = impl;
    return CL_OK;
}
extern "C" cl_status_t cl_pthread_mutex_lock(cl_pthread_mutex_t* mutex) {
    if (!mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    if (impl->distributed) {
        if (impl->lease) return CL_FAILED_PRECONDITION;
        auto result = impl->runtime->loommem.AcquireWriteBuffer(impl->object, 10000);
        if (!result.ok()) return ToCStatus(result.status());
        impl->lease = std::make_unique<cxloom::loommem::WriteBuffer>(std::move(result.value()));
        return CL_OK;
    }
    impl->value.lock(); return CL_OK;
}
extern "C" cl_status_t cl_pthread_mutex_trylock(cl_pthread_mutex_t* mutex) {
    if (!mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    if (impl->distributed) {
        if (impl->lease) return CL_UNAVAILABLE;
        auto result = impl->runtime->loommem.AcquireWriteBuffer(impl->object, 1);
        if (!result.ok()) return CL_UNAVAILABLE;
        impl->lease = std::make_unique<cxloom::loommem::WriteBuffer>(std::move(result.value()));
        return CL_OK;
    }
    return impl->value.try_lock() ? CL_OK : CL_UNAVAILABLE;
}
extern "C" cl_status_t cl_pthread_mutex_unlock(cl_pthread_mutex_t* mutex) {
    if (!mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    if (impl->distributed) {
        if (!impl->lease) return CL_FAILED_PRECONDITION;
        auto status = impl->runtime->loommem.ReleaseWriteBuffer(*impl->lease);
        if (status.ok()) impl->lease.reset();
        return ToCStatus(status);
    }
    impl->value.unlock(); return CL_OK;
}
extern "C" cl_status_t cl_pthread_mutex_destroy(cl_pthread_mutex_t* mutex) {
    if (!mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    if (impl->distributed) {
        if (impl->lease) return CL_FAILED_PRECONDITION;
        auto status = impl->runtime->loommem.FreeShared(impl->object);
        if (!status.ok()) return ToCStatus(status);
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
    condition->impl = impl; return CL_OK;
}
extern "C" cl_status_t cl_pthread_cond_wait(cl_pthread_cond_t* condition, cl_pthread_mutex_t* mutex) {
    if (!condition || !condition->impl || !mutex || !mutex->impl) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    auto* cond = static_cast<ClCond*>(condition->impl);
    if (impl->distributed != cond->distributed) return CL_FAILED_PRECONDITION;
    if (cond->distributed) {
        std::uint64_t observed = 0;
        if (cl_mem_read(cond->runtime, {cond->object.region_id, cond->object.offset}, 0,
                        &observed, sizeof(observed), 10000) != CL_OK) return CL_UNAVAILABLE;
        cond->waiters.fetch_add(1, std::memory_order_acq_rel);
        if (cl_pthread_mutex_unlock(mutex) != CL_OK) { cond->waiters.fetch_sub(1); return CL_FAILED_PRECONDITION; }
        for (;;) {
            std::uint64_t current = observed;
            auto status = cl_mem_read(cond->runtime, {cond->object.region_id, cond->object.offset}, 0,
                                      &current, sizeof(current), 10000);
            if (status != CL_OK) return status;
            if (current != observed) break;
            std::this_thread::yield();
        }
        auto result = cl_pthread_mutex_lock(mutex);
        cond->waiters.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }
    std::unique_lock<std::mutex> lock(impl->value, std::adopt_lock);
    cond->waiters.fetch_add(1, std::memory_order_acq_rel);
    static_cast<ClCond*>(condition->impl)->value.wait(lock); lock.release();
    cond->waiters.fetch_sub(1, std::memory_order_acq_rel);
    return CL_OK;
}
extern "C" cl_status_t cl_pthread_cond_timedwait(cl_pthread_cond_t* condition, cl_pthread_mutex_t* mutex,
                                                  uint64_t timeout_ms) {
    if (!condition || !condition->impl || !mutex || !mutex->impl || timeout_ms == 0) return CL_INVALID_ARGUMENT;
    auto* impl = static_cast<ClMutex*>(mutex->impl);
    auto* cond = static_cast<ClCond*>(condition->impl);
    if (cond->waiters.load(std::memory_order_acquire) != 0) return CL_FAILED_PRECONDITION;
    if (impl->distributed != cond->distributed) return CL_FAILED_PRECONDITION;
    if (cond->distributed) {
        std::uint64_t observed = 0;
        if (cl_mem_read(cond->runtime, {cond->object.region_id, cond->object.offset}, 0,
                        &observed, sizeof(observed), timeout_ms) != CL_OK) return CL_UNAVAILABLE;
        if (cl_pthread_mutex_unlock(mutex) != CL_OK) return CL_FAILED_PRECONDITION;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            std::uint64_t current = observed;
            auto status = cl_mem_read(cond->runtime, {cond->object.region_id, cond->object.offset}, 0,
                                      &current, sizeof(current), timeout_ms);
            if (status == CL_OK && current != observed) {
                auto result = cl_pthread_mutex_lock(mutex);
                cond->waiters.fetch_sub(1, std::memory_order_acq_rel);
                return result;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                cl_pthread_mutex_lock(mutex);
                cond->waiters.fetch_sub(1, std::memory_order_acq_rel);
                return CL_UNAVAILABLE;
            }
            std::this_thread::yield();
        }
    }
    std::unique_lock<std::mutex> lock(impl->value, std::adopt_lock);
    cond->waiters.fetch_add(1, std::memory_order_acq_rel);
    const auto ready = cond->value.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    lock.release();
    cond->waiters.fetch_sub(1, std::memory_order_acq_rel);
    return ready == std::cv_status::timeout ? CL_UNAVAILABLE : CL_OK;
}
extern "C" cl_status_t cl_pthread_cond_signal(cl_pthread_cond_t* condition) {
    if (!condition || !condition->impl) return CL_INVALID_ARGUMENT;
    auto* cond = static_cast<ClCond*>(condition->impl);
    if (cond->distributed) {
        auto result = cond->runtime->loommem.AcquireWriteBuffer(cond->object, 10000);
        if (!result.ok()) return ToCStatus(result.status());
        auto* value = static_cast<std::uint64_t*>(result.value().data()); ++*value;
        return ToCStatus(cond->runtime->loommem.ReleaseWriteBuffer(result.value()));
    }
    cond->value.notify_one(); return CL_OK;
}
extern "C" cl_status_t cl_pthread_cond_broadcast(cl_pthread_cond_t* condition) {
    return cl_pthread_cond_signal(condition);
}
extern "C" cl_status_t cl_pthread_cond_destroy(cl_pthread_cond_t* condition) {
    if (!condition || !condition->impl) return CL_INVALID_ARGUMENT;
    auto* cond = static_cast<ClCond*>(condition->impl);
    if (cond->distributed) {
        auto status = cond->runtime->loommem.FreeShared(cond->object);
        if (!status.ok()) return ToCStatus(status);
    }
    delete cond; condition->impl = nullptr; return CL_OK;
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
    if (runtime->config.host_count <= 1) {
        auto allocation = runtime->loommem.DescribeSharedAllocation({gptr.region_id, gptr.offset});
        if (!allocation.ok() || offset > allocation.value().bytes || bytes > allocation.value().bytes - offset)
            return allocation.ok() ? CL_INVALID_ARGUMENT : ToCStatus(allocation.status());
        auto address = runtime->loommem.ResolveLocal({gptr.region_id, gptr.offset});
        if (!address.ok()) return ToCStatus(address.status());
        std::memcpy(out_bytes, static_cast<std::byte*>(address.value()) + offset, bytes);
        return CL_OK;
    }
    auto result = runtime->loommem.AcquireReadRange({gptr.region_id, gptr.offset}, offset, bytes,
                                                     timeout_ms, cxloom::loommem::ReadConsistency::kPerBlock);
    if (!result.ok()) return ToCStatus(result.status());
    std::memcpy(out_bytes, result.value().data(), bytes);
    return CL_OK;
}

extern "C" cl_status_t cl_mem_write(cl_runtime_t* runtime, cl_gptr_t gptr, size_t offset,
                                      const void* bytes, size_t byte_count, uint64_t timeout_ms) {
    if (!runtime || !bytes || byte_count == 0 || timeout_ms == 0) return CL_INVALID_ARGUMENT;
    if (runtime->config.host_count <= 1) {
        auto allocation = runtime->loommem.DescribeSharedAllocation({gptr.region_id, gptr.offset});
        if (!allocation.ok() || offset > allocation.value().bytes || byte_count > allocation.value().bytes - offset)
            return allocation.ok() ? CL_INVALID_ARGUMENT : ToCStatus(allocation.status());
        auto address = runtime->loommem.ResolveLocal({gptr.region_id, gptr.offset});
        if (!address.ok()) return ToCStatus(address.status());
        std::memcpy(static_cast<std::byte*>(address.value()) + offset, bytes, byte_count);
        return CL_OK;
    }
    auto result = runtime->loommem.AcquireWriteRange({gptr.region_id, gptr.offset}, offset, byte_count,
                                                      timeout_ms, cxloom::loommem::WriteAtomicity::kPerBlock);
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
