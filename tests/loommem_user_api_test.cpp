#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <unistd.h>

#include "cxloom/loommem.h"

int main() {
    char path[] = "/tmp/cxloom-user-api-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);

    cxloom::CxloomConfig config;
    config.host_count = 2;
    config.shared_region_bytes = 192ULL << 20;
    config.shared_region_path = path;
    config.bootstrap_owner = true;
    config.create_region_file = true;
    config.queue_capacity_entries = 64;
    config.bootstrap_timeout_ms = 2000;

    auto host0 = cxloom::clInit(config);
    config.local_host_id = 1;
    config.bootstrap_owner = false;
    config.create_region_file = false;
    auto host1 = cxloom::clInit(config);
    if (!host0.ok() || !host1.ok()) {
        std::remove(path);
        return 1;
    }

    cxloom::AllocOptions options;
    options.bytes = 256;
    options.alignment = 64;
    options.coherence_granularity = cxloom::CoherenceGranularity::kFixedBlock;
    options.coherence_block_bytes = 64;
    const auto object = cxloom::clAlloc(*host0.value(), options);

    auto initial = object.ok() ? cxloom::clWrite(*host0.value(), object.value(), 2000)
                               : cxloom::Result<cxloom::WriteView>(object.status());
    if (initial.ok())
        std::fill_n(static_cast<std::byte*>(initial.value().data()), initial.value().size(), std::byte {0x11});
    bool passed = initial.ok() && initial.value().Commit().ok();

    auto range = object.ok()
                     ? cxloom::clWriteRange(*host1.value(), object.value(), 32, 128, 2000,
                                            cxloom::WriteAtomicity::kWholeRange)
                     : cxloom::Result<cxloom::WriteView>(object.status());
    if (range.ok())
        std::fill_n(static_cast<std::byte*>(range.value().data()), range.value().size(), std::byte {0x22});
    passed = passed && range.ok() && range.value().Commit().ok();

    auto aborted = object.ok() ? cxloom::clWriteRange(*host1.value(), object.value(), 0, 64, 2000)
                               : cxloom::Result<cxloom::WriteView>(object.status());
    if (aborted.ok())
        std::fill_n(static_cast<std::byte*>(aborted.value().data()), aborted.value().size(), std::byte {0x7f});
    passed = passed && aborted.ok() && aborted.value().Abort().ok();

    const auto read = object.ok()
                          ? cxloom::clReadRange(*host0.value(), object.value(), 0, 256, 2000,
                                               cxloom::ReadConsistency::kWholeRange)
                          : cxloom::Result<cxloom::ReadView>(object.status());
    passed = passed && read.ok() && read.value().size() == 256;
    if (read.ok()) {
        const auto* bytes = static_cast<const std::byte*>(read.value().data());
        for (std::size_t index = 0; index < read.value().size(); ++index) {
            const auto expected = index >= 32 && index < 160 ? std::byte {0x22} : std::byte {0x11};
            passed = passed && bytes[index] == expected;
        }
    }

    passed = passed && object.ok() && cxloom::clFree(*host0.value(), object.value()).ok();
    const auto destroy1 = cxloom::clDestroy(host1.value());
    const auto destroy0 = cxloom::clDestroy(host0.value());
    std::remove(path);
    return passed && destroy1.ok() && destroy0.ok() ? 0 : 1;
}
