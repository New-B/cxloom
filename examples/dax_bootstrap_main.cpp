#include <cstdlib>
#include <iostream>
#include <string>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

namespace {

std::uint64_t ParseBytes(const char* value, std::uint64_t fallback) {
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const auto number = std::strtoull(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    switch (*end) {
        case 'g':
        case 'G':
            return number << 30;
        case 'm':
        case 'M':
            return number << 20;
        case 'k':
        case 'K':
            return number << 10;
        case '\0':
            return number;
        default:
            return fallback;
    }
}

std::uint64_t ParseUnsigned(const char* value, std::uint64_t fallback) {
    return ParseBytes(value, fallback);
}

}  // namespace

int main() {
    const char* dax_path = std::getenv("CL_DAX_DEVICE");
    if (dax_path == nullptr || *dax_path == '\0') {
        std::cerr << "CL_DAX_DEVICE must name the shared devdax device\n";
        return 2;
    }

    cxloom::CxloomConfig config;
    config.local_host_id = static_cast<cxloom::HostId>(ParseUnsigned(std::getenv("CL_HOST_ID"), 0));
    config.host_count = static_cast<std::uint16_t>(ParseUnsigned(std::getenv("CL_HOST_COUNT"), 1));
    config.shared_region_bytes = ParseBytes(std::getenv("CL_SHARED_REGION_BYTES"), config.shared_region_bytes);
    config.shared_region_path = dax_path;
    config.bootstrap_owner = ParseUnsigned(std::getenv("CL_BOOTSTRAP_OWNER"), 0) != 0;

    cxloom::loommem::LoomMemRuntime runtime(config);
    const auto status = runtime.Initialize();
    if (!status.ok()) {
        std::cerr << "LoomMem bootstrap failed: " << status.message() << "\n";
        return 1;
    }

    std::cout << "host=" << config.local_host_id << " mapped=" << runtime.region_mapper().path()
              << " local_base=" << runtime.region_mapper().base()
              << " shared_data_offset=" << runtime.layout().shared_data.offset << "\n";
    return runtime.Finalize().ok() ? 0 : 1;
}
