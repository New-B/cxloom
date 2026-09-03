#include <cstdio>
#include <cstdint>
#include <unistd.h>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

int main() {
    char path[] = "/tmp/cxloom-region-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    close(fd);

    constexpr std::size_t kRegionBytes = 192ULL << 20;
    cxloom::CxloomConfig owner_config;
    owner_config.local_host_id = 0;
    owner_config.host_count = 2;
    owner_config.shared_region_bytes = kRegionBytes;
    owner_config.shared_region_path = path;
    owner_config.bootstrap_owner = true;
    owner_config.create_region_file = true;

    cxloom::loommem::LoomMemRuntime owner(owner_config);
    if (!owner.Initialize().ok()) {
        std::remove(path);
        return 1;
    }

    auto attach_config = owner_config;
    attach_config.local_host_id = 1;
    attach_config.bootstrap_owner = false;
    attach_config.create_region_file = false;
    cxloom::loommem::LoomMemRuntime attacher(attach_config);
    if (!attacher.Initialize().ok()) {
        owner.Finalize();
        std::remove(path);
        return 1;
    }

    const cxloom::GlobalPointer probe {0, owner.layout().shared_data.offset};
    const auto owner_address = owner.ResolveLocal(probe);
    const auto attacher_address = attacher.ResolveLocal(probe);
    if (!owner_address.ok() || !attacher_address.ok()) {
        attacher.Finalize();
        owner.Finalize();
        std::remove(path);
        return 1;
    }

    constexpr std::uint64_t kProbeValue = 0x43584C4F4F4D1234ULL;
    *static_cast<std::uint64_t*>(owner_address.value()) = kProbeValue;
    const bool shared_value_visible = *static_cast<std::uint64_t*>(attacher_address.value()) == kProbeValue;

    attacher.Finalize();
    owner.Finalize();
    std::remove(path);
    return shared_value_visible ? 0 : 1;
}
