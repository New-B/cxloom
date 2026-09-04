#include <array>
#include <cstddef>
#include <iostream>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

int main() {
    constexpr std::array<std::uint16_t, 6> kHostCounts {1, 2, 8, 16, 32, 64};
    std::size_t previous_capacity = 1025;
    for (const auto host_count : kHostCounts) {
        cxloom::CxloomConfig config;
        config.local_host_id = 0;
        config.host_count = host_count;
        config.shared_region_bytes = 192ULL << 20;
        config.queue_capacity_entries = 0;

        cxloom::loommem::LoomMemRuntime runtime(config);
        const auto status = runtime.Initialize();
        if (!status.ok()) {
            std::cerr << "host_count=" << host_count << ": " << status.message() << "\n";
            return 1;
        }
        const auto capacity = runtime.queue_capacity_entries();
        if (capacity == 0 || capacity > 1024 ||
            (host_count > 2 && capacity > previous_capacity)) {
            std::cerr << "invalid automatic queue capacity for host_count=" << host_count << "\n";
            return 1;
        }
        if (host_count > 1) {
            const auto first = runtime.GetQueue(0, 1);
            const auto last = runtime.GetQueue(host_count - 1, host_count - 2);
            if (!first.ok() || !last.ok()) {
                std::cerr << "dynamic queue matrix is incomplete for host_count=" << host_count << "\n";
                return 1;
            }
        }
        if (runtime.GetQueue(host_count, 0).ok() || !runtime.Finalize().ok())
            return 1;
        if (host_count > 1)
            previous_capacity = capacity;
    }
    return 0;
}
