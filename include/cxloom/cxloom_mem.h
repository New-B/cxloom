#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cl_runtime cl_runtime_t;

typedef struct {
    uint16_t home_host;
    uint64_t local_tid;
} cl_pthread_t;

typedef void *(*cl_pthread_start_routine)(void *);
// Explicit argument/result contract; schema_id is application-assigned and must
// change when the serialized layout or result-token meaning changes.
typedef struct {
    const char *name;
    cl_pthread_start_routine start_routine;
    uint32_t abi_version;
    uint32_t argument_bytes;
    uint64_t schema_id;
} cl_pthread_function_t;
typedef struct { void *impl; } cl_pthread_mutex_t;
typedef struct { void *impl; } cl_pthread_cond_t;

typedef struct {
    cl_runtime_t *runtime;
    uint64_t barrier_id;
    size_t local_participants;
} cl_pthread_barrier_t;

typedef struct {
    uint32_t region_id;
    uint64_t offset;
} cl_gptr_t;

typedef enum {
    CL_PLACEMENT_MEMORY_AWARE = 0,
    CL_PLACEMENT_ROUND_ROBIN = 1,
    CL_PLACEMENT_LEAST_LOADED = 2
} cl_placement_policy_t;

typedef struct {
    uint16_t local_host_id;
    uint16_t host_count;
    size_t shared_region_bytes;
    size_t coherence_granule_bytes;
    size_t queue_capacity_entries;
    const char *shared_region_path;
    uint8_t bootstrap_owner;
    uint8_t create_region_file;
    uint16_t reserved0;
    uint64_t bootstrap_timeout_ms;
    // Zero preserves the C++ runtime defaults.
    size_t replica_cache_capacity_entries;
    size_t replica_cache_capacity_bytes;
    cl_placement_policy_t placement_policy; // zero selects memory-aware placement
} cl_config_t;

typedef enum {
    CL_OK = 0,
    CL_INVALID_ARGUMENT,
    CL_NOT_FOUND,
    CL_ALREADY_EXISTS,
    CL_UNAVAILABLE,
    CL_FAILED_PRECONDITION,
    CL_UNIMPLEMENTED,
    CL_INTERNAL,
} cl_status_t;

// Creates a LoomMem runtime. The caller owns the returned handle.
cl_status_t cl_runtime_create(const cl_config_t *config, cl_runtime_t **runtime);
// Finalizes the runtime and reports lifecycle violations (unjoined threads,
// active barriers, staged writes, or undrained transport).
cl_status_t cl_runtime_finalize(cl_runtime_t *runtime);
void cl_runtime_destroy(cl_runtime_t *runtime);

// Allocates an object from the shared CXL data region. Shared DAX allocations
// are retired as whole objects. Their data and coherence-sidecar extents then
// return to independent free pools; coherence blocks are never freed alone.
cl_status_t cl_mem_alloc(cl_runtime_t *runtime, size_t bytes, size_t alignment, cl_gptr_t *out_gptr);
// CL_UNAVAILABLE may leave the object retiring; retry until reclamation completes.
// Every configured host must keep runtime progress alive during retirement.
cl_status_t cl_mem_free(cl_runtime_t *runtime, cl_gptr_t gptr);

// Resolves an offset-based global pointer into this host's mapping. This raw
// address is for bootstrap/mapping tests only until coherence acquire/release
// operations are added.
cl_status_t cl_mem_resolve_local(cl_runtime_t *runtime, cl_gptr_t gptr, void **out_address);
// Reads validate blocks independently; writes publish each block separately.
// Cross-block invariants require application synchronization.
cl_status_t cl_mem_read(cl_runtime_t *runtime, cl_gptr_t gptr, size_t offset,
                        void *out_bytes, size_t bytes, uint64_t timeout_ms);
cl_status_t cl_mem_write(cl_runtime_t *runtime, cl_gptr_t gptr, size_t offset,
                         const void *bytes, size_t byte_count, uint64_t timeout_ms);

// Collective bootstrap registration: every configured host supplies the same
// manifest (order may differ), binding names to its own local callback addresses.
// At most 256 entries, names 1..63 bytes, arguments 0..80 bytes, nonzero ABI/schema.
// The manifest is immutable. Timeout returns CL_UNAVAILABLE; retry the same list.
// Missing/mismatched peer descriptors reject registration before remote create.
cl_status_t cl_pthread_register_functions(cl_runtime_t *runtime,
                                         const cl_pthread_function_t *functions,
                                         size_t count, uint64_t timeout_ms);

// Pthreads-shaped LoomPar API. Placement, transport and execution host are
// selected internally. Multi-host calls require successful manifest registration.
// arg_bytes makes the argument representation explicit so
// a remote invocation never transmits a process-local pointer.
/* Range is relative to object; bytes=0 covers the remainder. Hints do not pin
 * objects for the child lifetime. Keep allocations alive until child completion. */
typedef enum { CL_MEMORY_READ = 0, CL_MEMORY_WRITE = 1, CL_MEMORY_READ_WRITE = 2 } cl_memory_access_t;
typedef struct {
    cl_gptr_t object;
    uint64_t offset, bytes;
    cl_memory_access_t access;
    double weight; /* finite, >0, <=1e6 */
} cl_working_set_entry_t;
cl_status_t cl_pthread_create_with_working_set(cl_runtime_t *runtime, cl_pthread_t *thread,
    cl_pthread_start_routine start_routine, const void *arg, size_t arg_bytes,
    const cl_working_set_entry_t *working_set, size_t count);

cl_status_t cl_pthread_create(cl_runtime_t *runtime, cl_pthread_t *thread,
                              cl_pthread_start_routine start_routine,
                              const void *arg, size_t arg_bytes);
cl_status_t cl_pthread_join(cl_runtime_t *runtime, cl_pthread_t thread, void **retval);
cl_status_t cl_pthread_detach(cl_runtime_t *runtime, cl_pthread_t thread);
// Cooperative migration checkpoint. Must be called by the current callback.
cl_status_t cl_pthread_migration_safe_point(cl_runtime_t *runtime);
cl_status_t cl_pthread_mutex_init(cl_runtime_t *runtime, cl_pthread_mutex_t *mutex);
cl_status_t cl_pthread_mutex_lock(cl_pthread_mutex_t *mutex);
cl_status_t cl_pthread_mutex_trylock(cl_pthread_mutex_t *mutex);
cl_status_t cl_pthread_mutex_unlock(cl_pthread_mutex_t *mutex);
cl_status_t cl_pthread_mutex_destroy(cl_pthread_mutex_t *mutex);
cl_status_t cl_pthread_cond_init(cl_runtime_t *runtime, cl_pthread_cond_t *condition);
cl_status_t cl_pthread_cond_wait(cl_pthread_cond_t *condition, cl_pthread_mutex_t *mutex);
cl_status_t cl_pthread_cond_timedwait(cl_pthread_cond_t *condition, cl_pthread_mutex_t *mutex,
                                      uint64_t timeout_ms);
cl_status_t cl_pthread_cond_signal(cl_pthread_cond_t *condition);
cl_status_t cl_pthread_cond_broadcast(cl_pthread_cond_t *condition);
cl_status_t cl_pthread_cond_destroy(cl_pthread_cond_t *condition);
// Share a distributed synchronization object's identity through application
// bootstrap. Attach creates a local handle to the same object, not a new object.
// Destroy attached handles on all hosts before destroying the allocating handle.
// The shared condition supports up to 64 simultaneous waiters (CL_UNAVAILABLE
// if full). Export/attach are only valid for multi-host runtimes.
cl_status_t cl_pthread_mutex_export(cl_pthread_mutex_t *mutex, cl_gptr_t *object);
cl_status_t cl_pthread_mutex_attach(cl_runtime_t *runtime, cl_pthread_mutex_t *mutex, cl_gptr_t object);
cl_status_t cl_pthread_cond_export(cl_pthread_cond_t *condition, cl_gptr_t *object);
cl_status_t cl_pthread_cond_attach(cl_runtime_t *runtime, cl_pthread_cond_t *condition, cl_gptr_t object);
cl_status_t cl_pthread_barrier_init(cl_runtime_t *runtime, cl_pthread_barrier_t *barrier,
                                    size_t local_participants);
cl_status_t cl_pthread_barrier_wait(cl_pthread_barrier_t *barrier);
cl_status_t cl_pthread_barrier_destroy(cl_pthread_barrier_t *barrier);

#ifdef __cplusplus
}
#endif
