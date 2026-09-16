#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "cxloom/cxloom_mem.h"

static _Atomic int calls;
static void *worker(void *arg) {
    if (arg && *(const int *)arg == 42) atomic_fetch_add(&calls, 1);
    return NULL;
}
int main(void) {
    cl_config_t config;
    memset(&config, 0, sizeof(config));
    config.host_count = 1;
    config.shared_region_bytes = 256ULL << 20;
    config.coherence_granule_bytes = 4096;
    config.replica_cache_capacity_entries = 1024;
    config.replica_cache_capacity_bytes = 64ULL << 20;
    config.bootstrap_timeout_ms = 10000;
    char path[] = "/tmp/cxloom-c-api-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    config.shared_region_path = path;
    config.bootstrap_owner = 1;
    config.create_region_file = 1;
    cl_runtime_t *runtime = NULL;
    cl_status_t status = cl_runtime_create(&config, &runtime);
    unlink(path);
    if (status != CL_OK) { fprintf(stderr, "create=%d\n", status); return 1; }
    int value = 42;
    cl_pthread_t thread;
    status = cl_pthread_create(runtime, &thread, worker, &value, sizeof(value));
    if (status != CL_OK) { fprintf(stderr, "pthread_create=%d\n", status); return 1; }
    void *retval = NULL;
    status = cl_pthread_join(runtime, thread, &retval);
    if (status != CL_OK || calls != 1 || retval != NULL) { fprintf(stderr, "join=%d calls=%d\n", status, calls); return 1; }
    cl_pthread_barrier_t barrier;
    if (cl_pthread_barrier_init(runtime, &barrier, 1) != CL_OK) return 1;
    if (cl_pthread_barrier_wait(&barrier) != CL_OK) return 1;
    if (cl_pthread_barrier_destroy(&barrier) != CL_OK) return 1;
    cl_runtime_destroy(runtime);
    puts("opaque cl_pthread create/join/barrier API passed");
    return 0;
}
