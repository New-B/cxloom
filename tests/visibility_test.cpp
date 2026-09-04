#include <array>
#include <cstddef>
#include <iostream>

#include "cxloom/loommem/visibility.h"

int main() {
    using cxloom::loommem::AcquireData;
    using cxloom::loommem::ParseVisibilityMode;
    using cxloom::loommem::PublishData;
    using cxloom::loommem::VisibilityMode;
    using cxloom::loommem::VisibilityModeName;

    const std::array<const char*, 4> names {"release", "seq_cst", "clflush", "clwb"};
    const std::array<VisibilityMode, 4> modes {
        VisibilityMode::kReleaseAcquire,
        VisibilityMode::kSequentiallyConsistent,
        VisibilityMode::kClflush,
        VisibilityMode::kClwb,
    };

    alignas(64) std::array<std::byte, 129> data {};
    for (std::size_t index = 0; index < modes.size(); ++index) {
        const auto parsed = ParseVisibilityMode(names[index]);
        if (!parsed.ok() || parsed.value() != modes[index] ||
            std::string(VisibilityModeName(modes[index])) != names[index]) {
            std::cerr << "visibility mode parsing or naming failed\n";
            return 1;
        }
        const auto publish = PublishData(data.data() + 1, data.size() - 1, modes[index]);
        const auto acquire = AcquireData(data.data() + 1, data.size() - 1, modes[index]);
        if (modes[index] == VisibilityMode::kClwb && !publish.ok() && !acquire.ok()) {
            continue;
        }
        if (!publish.ok() || !acquire.ok()) {
            std::cerr << "visibility mode unexpectedly unavailable\n";
            return 1;
        }
    }

    if (ParseVisibilityMode("unknown").ok() ||
        PublishData(nullptr, data.size(), VisibilityMode::kReleaseAcquire).ok() ||
        AcquireData(data.data(), 0, VisibilityMode::kReleaseAcquire).ok()) {
        std::cerr << "visibility validation accepted invalid input\n";
        return 1;
    }
    return 0;
}
