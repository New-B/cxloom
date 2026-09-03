#include <cstdlib>
#include <iostream>
#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

namespace {
std::uint64_t Parse(const char* value, std::uint64_t fallback) {
    if (value == nullptr || *value == '\0') return fallback;
    char* end = nullptr;
    const auto number = std::strtoull(value, &end, 10);
    if (end == value) return fallback;
    if (*end == 'G' || *end == 'g') return number << 30;
    if (*end == 'M' || *end == 'm') return number << 20;
    if (*end == 'K' || *end == 'k') return number << 10;
    return *end == '\0' ? number : fallback;
}
}

int main() {
    const char* dax_path = std::getenv("CL_DAX_DEVICE");
    if (dax_path == nullptr || *dax_path == '\0') {
        std::cerr << "CL_DAX_DEVICE must name the shared devdax device\n";
        return 2;
    }
    cxloom::CxloomConfig config;
    config.local_host_id = static_cast<cxloom::HostId>(Parse(std::getenv("CL_HOST_ID"), 0));
    config.host_count = static_cast<std::uint16_t>(Parse(std::getenv("CL_HOST_COUNT"), 1));
    config.shared_region_bytes = Parse(std::getenv("CL_SHARED_REGION_BYTES"), config.shared_region_bytes);
    config.shared_region_path = dax_path;
    config.bootstrap_owner = Parse(std::getenv("CL_BOOTSTRAP_OWNER"), 0) != 0;

    cxloom::loommem::LoomMemRuntime runtime(config);
    auto status = runtime.Initialize();
    if (!status.ok()) {
        std::cerr << "host " << config.local_host_id << " initialization failed: " << status.message() << "\n";
        return 1;
    }
    constexpr std::uint64_t kProbeMagic = 0x43584c4f4f4d0000ULL;
    status = runtime.PublishBootstrapProbe(kProbeMagic | config.local_host_id);
    if (status.ok()) status = runtime.WaitForAllHosts(config.bootstrap_timeout_ms);
    if (!status.ok()) {
        std::cerr << "host " << config.local_host_id << " rendezvous failed: " << status.message() << "\n";
        return 1;
    }
    for (cxloom::HostId host = 0; host < config.host_count; ++host) {
        const auto probe = runtime.ReadBootstrapProbe(host);
        if (!probe.ok() || probe.value() != (kProbeMagic | host)) {
            std::cerr << "host " << config.local_host_id << " observed invalid probe from host " << host << "\n";
            return 1;
        }
    }
    std::cout << "host=" << config.local_host_id << " joined=" << runtime.joined_host_count()
              << "/" << config.host_count << " local_base=" << runtime.region_mapper().base()
              << " bootstrap_rendezvous=ok\n";
    return runtime.Finalize().ok() ? 0 : 1;
}
