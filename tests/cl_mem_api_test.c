#include <stddef.h>

#include "cxloom/cxloom.h"

int main(void) {
    cl_config_t config = {
        .local_host_id = 0,
        .host_count = 1,
        .shared_region_bytes = 256ULL << 20,
        .per_host_extent_bytes = 64ULL << 20,
        .coherence_granule_bytes = 4096,
        .queue_capacity_entries = 64,
    };
    cl_runtime_t *runtime = NULL;
    if (cl_runtime_create(&config, &runtime) != CL_OK) {
        return 1;
    }
    cl_gptr_t value;
    const int result = cl_mem_alloc(runtime, 128, 64, &value) == CL_OK &&
                       cl_mem_free(runtime, value) == CL_OK;
    cl_runtime_destroy(runtime);
    return result ? 0 : 1;
}
