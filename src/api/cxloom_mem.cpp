#include "cxloom/cxloom_mem.h"

#include <new>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

struct cl_runtime {
    explicit cl_runtime(cxloom::CxloomConfig value) : loommem(std::move(value)) {}

    cxloom::loommem::LoomMemRuntime loommem;
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
    result.per_host_extent_bytes = config.per_host_extent_bytes;
    result.coherence_granule_bytes = config.coherence_granule_bytes;
    result.queue_capacity_entries = config.queue_capacity_entries;
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
        runtime->loommem.Finalize();
        delete runtime;
    }
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
