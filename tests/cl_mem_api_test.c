#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>

#include "cxloom/cxloom.h"

int main(void) {
    cl_config_t config = {
        .local_host_id = 0,
        .host_count = 1,
        .shared_region_bytes = 256ULL << 20,
        .coherence_granule_bytes = 4096,
        .queue_capacity_entries = 64,
    };
    char path[] = "/tmp/cxloom-c-api-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    config.shared_region_path = path;
    config.bootstrap_owner = 1;
    config.create_region_file = 1;
    cl_runtime_t *runtime = NULL;
    const cl_status_t status = cl_runtime_create(&config, &runtime);
    unlink(path);
    if (status != CL_OK) {
        return 1;
    }
    cl_gptr_t value;
    int written = 42, read = 0;
    const int result = cl_mem_alloc(runtime, 128, 64, &value) == CL_OK &&
                       cl_mem_write(runtime, value, 0, &written, sizeof(written), 2000) == CL_OK &&
                       cl_mem_read(runtime, value, 0, &read, sizeof(read), 2000) == CL_OK &&
                       read == written && cl_mem_free(runtime, value) == CL_OK;
    cl_runtime_destroy(runtime);
    return result ? 0 : 1;
}
