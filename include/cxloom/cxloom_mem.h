#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cl_runtime cl_runtime_t;

typedef struct {
    uint32_t region_id;
    uint64_t offset;
} cl_gptr_t;

typedef struct {
    uint16_t local_host_id;
    uint16_t host_count;
    size_t shared_region_bytes;
    // Legacy process-private placement hint; ignored by the shared allocator.
    size_t per_host_extent_bytes;
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
void cl_runtime_destroy(cl_runtime_t *runtime);

// Allocates an object from the shared CXL data region. Shared DAX allocations
// use a global append-only pool and cannot yet be freed or reused.
cl_status_t cl_mem_alloc(cl_runtime_t *runtime, size_t bytes, size_t alignment, cl_gptr_t *out_gptr);
cl_status_t cl_mem_free(cl_runtime_t *runtime, cl_gptr_t gptr);

// Resolves an offset-based global pointer into this host's mapping. This raw
// address is for bootstrap/mapping tests only until coherence acquire/release
// operations are added.
cl_status_t cl_mem_resolve_local(cl_runtime_t *runtime, cl_gptr_t gptr, void **out_address);

#ifdef __cplusplus
}
#endif
