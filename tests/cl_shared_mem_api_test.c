#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "cxloom/cxloom.h"

int main(void) {
    char path[] = "/tmp/cxloom-c-api-region-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);

    cl_config_t owner_config = {
        .local_host_id = 0,
        .host_count = 2,
        .shared_region_bytes = 192ULL << 20,
        .coherence_granule_bytes = 4096,
        .queue_capacity_entries = 64,
        .shared_region_path = path,
        .bootstrap_owner = 1,
        .create_region_file = 1,
        .bootstrap_timeout_ms = 1000,
    };

    cl_runtime_t* owner = NULL;
    cl_runtime_t* attacher = NULL;
    int success = 0;
    if (cl_runtime_create(&owner_config, &owner) != CL_OK)
        goto done;

    cl_config_t attach_config = owner_config;
    attach_config.local_host_id = 1;
    attach_config.bootstrap_owner = 0;
    attach_config.create_region_file = 0;
    if (cl_runtime_create(&attach_config, &attacher) != CL_OK)
        goto done;

    cl_gptr_t owner_object;
    cl_gptr_t attacher_object;
    if (cl_mem_alloc(owner, sizeof(uint64_t), _Alignof(uint64_t), &owner_object) != CL_OK ||
        cl_mem_alloc(attacher, sizeof(uint64_t), _Alignof(uint64_t), &attacher_object) != CL_OK ||
        owner_object.offset == attacher_object.offset) {
        goto done;
    }

    // Starting the pthread layer starts each runtime's queue poller. Exercise
    // the coherence path through the C GPtr API rather than raw mappings.
    cl_pthread_mutex_t owner_mutex = {0};
    cl_pthread_mutex_t attacher_mutex = {0};
    if (cl_pthread_mutex_init(owner, &owner_mutex) != CL_OK ||
        cl_pthread_mutex_init(attacher, &attacher_mutex) != CL_OK)
        goto done;
    uint64_t wire_value = UINT64_C(0x43584c4f4f4d0042);
    uint64_t observed_value = 0;
    if (cl_mem_write(owner, owner_object, 0, &wire_value, sizeof(wire_value), 5000) != CL_OK ||
        cl_mem_read(attacher, owner_object, 0, &observed_value, sizeof(observed_value), 5000) != CL_OK ||
        observed_value != wire_value)
        goto done;
    cl_pthread_mutex_destroy(&attacher_mutex);
    cl_pthread_mutex_destroy(&owner_mutex);

    void* owner_local = NULL;
    void* owner_remote = NULL;
    void* attacher_local = NULL;
    void* attacher_remote = NULL;
    if (cl_mem_resolve_local(owner, owner_object, &owner_local) != CL_OK ||
        cl_mem_resolve_local(attacher, owner_object, &owner_remote) != CL_OK ||
        cl_mem_resolve_local(attacher, attacher_object, &attacher_local) != CL_OK ||
        cl_mem_resolve_local(owner, attacher_object, &attacher_remote) != CL_OK) {
        goto done;
    }

    *(uint64_t*)owner_local = UINT64_C(0x43584c4f4f4d0000);
    *(uint64_t*)attacher_local = UINT64_C(0x43584c4f4f4d0001);
    if (*(uint64_t*)owner_remote != UINT64_C(0x43584c4f4f4d0000) ||
        *(uint64_t*)attacher_remote != UINT64_C(0x43584c4f4f4d0001) ||
        cl_mem_free(owner, owner_object) != CL_OK) {
        goto done;
    }
    success = 1;

done:
    cl_runtime_destroy(attacher);
    cl_runtime_destroy(owner);
    unlink(path);
    return success ? 0 : 1;
}
