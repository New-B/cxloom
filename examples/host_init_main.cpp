#include <atomic>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

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

struct alignas(64) TestObject {
    std::uint64_t magic;
    std::uint64_t owner_host;
    std::uint64_t object_offset;
    std::uint64_t checksum;
    std::uint64_t padding[4];
};

constexpr std::uint64_t kObjectMagic = 0x43584c4f4f4d4f42ULL;

std::uint64_t Checksum(cxloom::HostId host, std::uint64_t offset) {
    return kObjectMagic ^ static_cast<std::uint64_t>(host) ^ offset;
}

}  // namespace

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

    const auto allocation = runtime.AllocateShared(sizeof(TestObject), alignof(TestObject));
    if (!allocation.ok()) {
        std::cerr << "host " << config.local_host_id << " allocation failed: " << allocation.status().message() << "\n";
        return 1;
    }
    const auto local = runtime.ResolveLocal(allocation.value());
    if (!local.ok()) {
        std::cerr << "host " << config.local_host_id << " local resolution failed: " << local.status().message()
                  << "\n";
        return 1;
    }

    const std::size_t extra_sizes[] = {256, 4096, 64ULL << 10};
    const std::size_t extra_alignments[] = {64, 4096, 4096};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto extra = runtime.AllocateShared(extra_sizes[index], extra_alignments[index]);
        if (!extra.ok() || extra.value().offset % extra_alignments[index] != 0) {
            std::cerr << "host " << config.local_host_id << " extra allocation failed\n";
            return 1;
        }
        const auto description = runtime.DescribeSharedAllocation(extra.value());
        if (!description.ok() || description.value().owner_host != config.local_host_id ||
            description.value().bytes != extra_sizes[index] ||
            description.value().alignment != extra_alignments[index]) {
            std::cerr << "host " << config.local_host_id << " allocation descriptor mismatch\n";
            return 1;
        }
    }

    auto* object = static_cast<TestObject*>(local.value());
    *object = TestObject {kObjectMagic,
                          config.local_host_id,
                          allocation.value().offset,
                          Checksum(config.local_host_id, allocation.value().offset),
                          {0, 0, 0, 0}};
    std::atomic_thread_fence(std::memory_order_release);
    status = runtime.PublishSharedObject(allocation.value(), sizeof(TestObject));
    if (status.ok())
        status = runtime.WaitForAllSharedObjects(config.bootstrap_timeout_ms);
    if (!status.ok()) {
        std::cerr << "host " << config.local_host_id << " object rendezvous failed: " << status.message() << "\n";
        return 1;
    }

    std::vector<cxloom::loommem::PublishedSharedObject> published;
    published.reserve(config.host_count);
    for (cxloom::HostId host = 0; host < config.host_count; ++host) {
        auto entry = runtime.ReadPublishedSharedObject(host);
        if (!entry.ok()) {
            std::cerr << "host " << config.local_host_id << " cannot read allocation from host " << host << ": "
                      << entry.status().message() << "\n";
            return 1;
        }
        published.push_back(entry.value());
    }

    for (std::size_t lhs = 0; lhs < published.size(); ++lhs) {
        const auto lhs_begin = published[lhs].gptr.offset;
        const auto lhs_end = lhs_begin + published[lhs].bytes;
        for (std::size_t rhs = lhs + 1; rhs < published.size(); ++rhs) {
            const auto rhs_begin = published[rhs].gptr.offset;
            const auto rhs_end = rhs_begin + published[rhs].bytes;
            if (lhs_begin < rhs_end && rhs_begin < lhs_end) {
                std::cerr << "allocations from hosts " << lhs << " and " << rhs << " overlap\n";
                return 1;
            }
        }

        const auto remote = runtime.ResolveLocal(published[lhs].gptr);
        if (!remote.ok()) {
            std::cerr << "host " << config.local_host_id << " cannot resolve allocation from host " << lhs << ": "
                      << remote.status().message() << "\n";
            return 1;
        }
        const auto* remote_object = static_cast<const TestObject*>(remote.value());
        if (remote_object->magic != kObjectMagic || remote_object->owner_host != lhs ||
            remote_object->object_offset != lhs_begin ||
            remote_object->checksum != Checksum(static_cast<cxloom::HostId>(lhs), lhs_begin)) {
            std::cerr << "host " << config.local_host_id << " observed invalid object contents from host " << lhs
                      << "\n";
            return 1;
        }
    }

    std::cout << "host=" << config.local_host_id << " joined=" << runtime.joined_host_count() << "/"
              << config.host_count << " local_base=" << runtime.region_mapper().base()
              << " allocations_per_host=4 allocated_offset=" << allocation.value().offset
              << " allocations_non_overlapping=ok remote_reads=ok\n";
    return runtime.Finalize().ok() ? 0 : 1;
}
