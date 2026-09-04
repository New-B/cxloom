#include <chrono>
#include <cstdlib>
#include <iostream>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"
#include "cxloom/loommem/visibility.h"

namespace {

std::uint64_t Parse(const char* value, std::uint64_t fallback) {
    if (value == nullptr || *value == '\0')
        return fallback;
    char* end = nullptr;
    const auto number = std::strtoull(value, &end, 10);
    if (end == value)
        return fallback;
    if (*end == 'G' || *end == 'g')
        return number << 30;
    if (*end == 'M' || *end == 'm')
        return number << 20;
    if (*end == 'K' || *end == 'k')
        return number << 10;
    return *end == '\0' ? number : fallback;
}

struct alignas(64) VisibilityRecord {
    std::uint64_t begin_sequence;
    std::uint64_t writer_host;
    std::uint64_t checksum;
    std::uint64_t payload[508];
    std::uint64_t end_sequence;
};

std::uint64_t ValueFor(cxloom::HostId host, std::uint64_t sequence, std::size_t index) {
    auto value = sequence * UINT64_C(0x9e3779b97f4a7c15);
    value ^= (static_cast<std::uint64_t>(host) + 1) * UINT64_C(0xbf58476d1ce4e5b9);
    value ^= (index + 1) * UINT64_C(0x94d049bb133111eb);
    return value;
}

void FillRecord(VisibilityRecord& record, cxloom::HostId host, std::uint64_t sequence) {
    record.begin_sequence = sequence;
    record.writer_host = host;
    std::uint64_t checksum = sequence ^ host;
    for (std::size_t index = 0; index < 508; ++index) {
        record.payload[index] = ValueFor(host, sequence, index);
        checksum ^= record.payload[index];
    }
    record.checksum = checksum;
    record.end_sequence = sequence;
}

bool VerifyRecord(const VisibilityRecord& record, cxloom::HostId host, std::uint64_t sequence) {
    if (record.begin_sequence != sequence || record.end_sequence != sequence || record.writer_host != host) {
        return false;
    }
    std::uint64_t checksum = sequence ^ host;
    for (std::size_t index = 0; index < 508; ++index) {
        if (record.payload[index] != ValueFor(host, sequence, index))
            return false;
        checksum ^= record.payload[index];
    }
    return record.checksum == checksum;
}

}  // namespace

int main() {
    const char* dax_path = std::getenv("CL_DAX_DEVICE");
    const char* mode_name = std::getenv("CL_VISIBILITY_MODE");
    if (dax_path == nullptr || *dax_path == '\0' || mode_name == nullptr || *mode_name == '\0') {
        std::cerr << "CL_DAX_DEVICE and CL_VISIBILITY_MODE are required\n";
        return 2;
    }
    const auto mode = cxloom::loommem::ParseVisibilityMode(mode_name);
    if (!mode.ok()) {
        std::cerr << mode.status().message() << "\n";
        return 2;
    }
    const auto mode_status = cxloom::loommem::ValidateVisibilityMode(mode.value());
    if (!mode_status.ok()) {
        std::cerr << "visibility mode unavailable: " << mode_status.message() << "\n";
        return 2;
    }

    cxloom::CxloomConfig config;
    config.local_host_id = static_cast<cxloom::HostId>(Parse(std::getenv("CL_HOST_ID"), 0));
    config.host_count = static_cast<std::uint16_t>(Parse(std::getenv("CL_HOST_COUNT"), 1));
    config.shared_region_bytes = Parse(std::getenv("CL_SHARED_REGION_BYTES"), config.shared_region_bytes);
    config.bootstrap_timeout_ms = Parse(std::getenv("CL_VISIBILITY_TIMEOUT_MS"), 30000);
    config.shared_region_path = dax_path;
    config.bootstrap_owner = Parse(std::getenv("CL_BOOTSTRAP_OWNER"), 0) != 0;
    const auto iterations = Parse(std::getenv("CL_VISIBILITY_ITERATIONS"), 1000);

    cxloom::loommem::LoomMemRuntime runtime(config);
    auto status = runtime.Initialize();
    if (!status.ok()) {
        std::cerr << "runtime initialization failed: " << status.message() << "\n";
        return 1;
    }

    const auto allocation = runtime.AllocateShared(sizeof(VisibilityRecord), alignof(VisibilityRecord));
    if (!allocation.ok()) {
        std::cerr << "visibility record allocation failed: " << allocation.status().message() << "\n";
        return 1;
    }
    const auto local = runtime.ResolveLocal(allocation.value());
    if (!local.ok()) {
        std::cerr << "visibility record resolution failed: " << local.status().message() << "\n";
        return 1;
    }
    auto* local_record = static_cast<VisibilityRecord*>(local.value());
    FillRecord(*local_record, config.local_host_id, 0);
    status = cxloom::loommem::PublishData(local_record, sizeof(*local_record), mode.value());
    if (status.ok())
        status = runtime.PublishSharedObject(allocation.value(), sizeof(*local_record), mode.value());
    if (status.ok())
        status = runtime.WaitForAllSharedObjects(config.bootstrap_timeout_ms, mode.value());
    if (!status.ok()) {
        std::cerr << "initial object publication failed: " << status.message() << "\n";
        return 1;
    }

    std::uint64_t local_errors = 0;
    const auto test_start = std::chrono::steady_clock::now();
    for (std::uint64_t sequence = 1; sequence <= iterations; ++sequence) {
        FillRecord(*local_record, config.local_host_id, sequence);
        status = cxloom::loommem::PublishData(local_record, sizeof(*local_record), mode.value());
        if (status.ok())
            status = runtime.PublishVisibilitySequence(sequence, mode.value());
        if (status.ok())
            status = runtime.WaitForVisibilitySequence(sequence, config.bootstrap_timeout_ms, mode.value());
        if (!status.ok()) {
            std::cerr << "publication barrier failed at iteration " << sequence << ": " << status.message() << "\n";
            return 1;
        }

        for (cxloom::HostId host = 0; host < config.host_count; ++host) {
            const auto published = runtime.ReadPublishedSharedObject(host);
            if (!published.ok()) {
                ++local_errors;
                continue;
            }
            const auto remote = runtime.ResolveLocal(published.value().gptr);
            if (!remote.ok()) {
                ++local_errors;
                continue;
            }
            const auto acquire_status =
                cxloom::loommem::AcquireData(remote.value(), published.value().bytes, mode.value());
            if (!acquire_status.ok() ||
                !VerifyRecord(*static_cast<const VisibilityRecord*>(remote.value()), host, sequence)) {
                ++local_errors;
            }
        }

        status = runtime.PublishObservedSequence(sequence, local_errors, mode.value());
        if (status.ok())
            status = runtime.WaitForObservedSequence(sequence, config.bootstrap_timeout_ms, mode.value());
        if (!status.ok()) {
            std::cerr << "observation barrier failed at iteration " << sequence << ": " << status.message() << "\n";
            return 1;
        }
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - test_start)
                                .count();
    const auto total_errors = runtime.visibility_error_count();
    std::cout << "host=" << config.local_host_id << " mode=" << cxloom::loommem::VisibilityModeName(mode.value())
              << " iterations=" << iterations << " elapsed_ms=" << elapsed_ms << " local_errors=" << local_errors
              << " total_errors=" << total_errors
              << "\n";
    runtime.Finalize();
    return total_errors == 0 ? 0 : 1;
}
