#include "cxloom/cxloom_mem.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { std::cerr << "host=" << host << " line=" << __LINE__ << ": " << #x << std::endl; std::exit(1); } } while (0)
struct Objects { cl_gptr_t mutex, condition, counter; };
int main() {
    const unsigned host = std::strtoul(std::getenv("CL_HOST_ID"), nullptr, 10);
    cl_config_t config{};
    config.local_host_id = host; config.host_count = 16;
    config.shared_region_bytes = 192ULL << 20; config.coherence_granule_bytes = 4096;
    config.shared_region_path = std::getenv("CL_DAX_DEVICE");
    config.bootstrap_owner = host == 0; config.create_region_file = 1;
    cl_runtime_t* runtime = nullptr;
    CHECK(cl_runtime_create(&config, &runtime) == CL_OK);
    std::cout << "region ready host=" << host << std::endl;
    cl_pthread_barrier_t barrier{};
    CHECK(cl_pthread_barrier_init(runtime, &barrier, 1) == CL_OK);
    cl_pthread_mutex_t mutex{}; cl_pthread_cond_t condition{};
    Objects objects{};
    const std::string metadata = std::string(config.shared_region_path) + ".sync";
    if (host == 0) {
        CHECK(cl_pthread_mutex_init(runtime, &mutex) == CL_OK);
        CHECK(cl_pthread_cond_init(runtime, &condition) == CL_OK);
        CHECK(cl_pthread_mutex_export(&mutex, &objects.mutex) == CL_OK);
        CHECK(cl_pthread_cond_export(&condition, &objects.condition) == CL_OK);
        CHECK(cl_mem_alloc(runtime, 64, 64, &objects.counter) == CL_OK);
        std::uint64_t value[2] = {};
        CHECK(cl_mem_write(runtime, objects.counter, 0, value, sizeof(value), 10000) == CL_OK);
        auto* file = std::fopen(metadata.c_str(), "wb"); CHECK(file);
        CHECK(std::fwrite(&objects, sizeof(objects), 1, file) == 1); std::fclose(file);
    }
    CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK);
    if (host != 0) {
        auto* file = std::fopen(metadata.c_str(), "rb"); CHECK(file);
        CHECK(std::fread(&objects, sizeof(objects), 1, file) == 1); std::fclose(file);
        CHECK(cl_pthread_mutex_attach(runtime, &mutex, objects.mutex) == CL_OK);
        CHECK(cl_pthread_cond_attach(runtime, &condition, objects.condition) == CL_OK);
        cl_pthread_cond_t wrong{};
        CHECK(cl_pthread_cond_attach(runtime, &wrong, objects.mutex) == CL_INVALID_ARGUMENT);
    }
    CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK);
    for (unsigned round = 0; round < 3; ++round) {
        if (host != 0) {
            CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
            std::uint64_t value = 0;
            CHECK(cl_mem_read(runtime, objects.counter, 0, &value, sizeof(value), 10000) == CL_OK);
            ++value;
            CHECK(cl_mem_write(runtime, objects.counter, 0, &value, sizeof(value), 10000) == CL_OK);
            CHECK(cl_pthread_cond_wait(&condition, &mutex) == CL_OK);
            CHECK(cl_mem_read(runtime, objects.counter, 8, &value, sizeof(value), 10000) == CL_OK);
            ++value;
            CHECK(cl_mem_write(runtime, objects.counter, 8, &value, sizeof(value), 10000) == CL_OK);
            CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
        } else {
            for (;;) {
                CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
                std::uint64_t value = 0;
                CHECK(cl_mem_read(runtime, objects.counter, 0, &value, sizeof(value), 10000) == CL_OK);
                if (value == 15 * (round + 1)) {
                    CHECK(cl_pthread_cond_signal(&condition) == CL_OK);
                    CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK); break;
                }
                CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (;;) {
                CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
                std::uint64_t woken = 0;
                CHECK(cl_mem_read(runtime, objects.counter, 8, &woken, sizeof(woken), 10000) == CL_OK);
                if (woken > 15 * round) {
                    CHECK(woken == 15 * round + 1);
                    CHECK(cl_pthread_cond_broadcast(&condition) == CL_OK);
                    CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK); break;
                }
                CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK);
        CHECK(cl_pthread_mutex_lock(&mutex) == CL_OK);
        CHECK(cl_pthread_cond_timedwait(&condition, &mutex, 5) == CL_UNAVAILABLE);
        CHECK(cl_pthread_mutex_trylock(&mutex) == CL_UNAVAILABLE);
        CHECK(cl_pthread_mutex_unlock(&mutex) == CL_OK);
        CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK);
    }
    if (host != 0) {
        CHECK(cl_pthread_cond_destroy(&condition) == CL_OK);
        CHECK(cl_pthread_mutex_destroy(&mutex) == CL_OK);
    }
    CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK);
    if (host == 0) {
        CHECK(cl_pthread_cond_destroy(&condition) == CL_OK);
        CHECK(cl_pthread_mutex_destroy(&mutex) == CL_OK);
        CHECK(cl_mem_free(runtime, objects.counter) == CL_OK);
        unlink(metadata.c_str());
    }
    CHECK(cl_pthread_barrier_wait(&barrier) == CL_OK);
    CHECK(cl_pthread_barrier_destroy(&barrier) == CL_OK);
    CHECK(cl_runtime_finalize(runtime) == CL_OK);
    cl_runtime_destroy(runtime);
    std::cout << "PASS distributed condition host=" << host << std::endl;
}
