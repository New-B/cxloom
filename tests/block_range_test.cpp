#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>
#include <unistd.h>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

int main() {
    char path[] = "/tmp/cxloom-block-range-XXXXXX";
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
    config.default_coherence_granularity = cxloom::CoherenceGranularity::kFixedBlock;
    config.coherence_granule_bytes = 64;
    config.replica_cache_capacity_entries = 2;
    config.replica_cache_capacity_bytes = 128;
    config.bootstrap_timeout_ms = 25;

    cxloom::loommem::LoomMemRuntime host0(config);
    if (!host0.Initialize().ok()) {
        std::remove(path);
        return 1;
    }
    config.local_host_id = 1;
    config.bootstrap_owner = false;
    config.create_region_file = false;
    cxloom::loommem::LoomMemRuntime host1(config);
    if (!host1.Initialize().ok()) {
        host0.Finalize();
        std::remove(path);
        return 1;
    }

    cxloom::loommem::AllocationOptions options;
    options.bytes = 256;
    options.alignment = 64;
    options.coherence_granularity = cxloom::CoherenceGranularity::kFixedBlock;
    options.coherence_block_bytes = 64;
    const auto object = host0.AllocateShared(options);
    bool passed = object.ok() && host0.StartQueuePoller().ok() && host1.StartQueuePoller().ok();
    const auto info = object.ok() ? host0.DescribeSharedAllocation(object.value())
                                  : cxloom::Result<cxloom::loommem::AllocationInfo>(object.status());
    passed = passed && info.ok() && info.value().coherence_block_bytes == 64 &&
             info.value().coherence_block_count == 4;

    // Different blocks have independent token ownership and can be held at
    // the same time by different hosts.
    const auto request0 = host0.RequestWriteToken(object.value(), 0);
    const auto lease0 = request0.ok() ? host0.WaitForWriteToken(request0.value(), 2000)
                                      : cxloom::Result<cxloom::loommem::TokenLease>(request0.status());
    const auto request1 = host1.RequestWriteToken(object.value(), 1);
    const auto lease1 = request1.ok() ? host1.WaitForWriteToken(request1.value(), 2000)
                                      : cxloom::Result<cxloom::loommem::TokenLease>(request1.status());
    passed = passed && lease0.ok() && lease1.ok() && lease0.value().block_index == 0 &&
             lease1.value().block_index == 1;
    if (lease0.ok())
        passed = host0.ReleaseWriteToken(lease0.value()).ok() && passed;
    if (lease1.ok())
        passed = host1.ReleaseWriteToken(lease1.value()).ok() && passed;

    auto initial = host0.AcquireWriteRange(object.value(), 0, 256, 2000);
    if (initial.ok())
        std::fill(initial.value().storage->begin(), initial.value().storage->end(), std::byte {0x11});
    passed = passed && initial.ok() && host0.ReleaseWriteBuffer(initial.value()).ok();

    // A partial range crosses three blocks. Bytes outside it must survive the
    // per-block read-modify-write publication.
    auto partial = host1.AcquireWriteRange(object.value(), 32, 128, 2000);
    if (partial.ok())
        std::fill(partial.value().storage->begin(), partial.value().storage->end(), std::byte {0x7a});
    passed = passed && partial.ok() && partial.value().leases.size() == 3 &&
             host1.ReleaseWriteBuffer(partial.value()).ok();
    const auto snapshot = host0.AcquireReadRange(object.value(), 0, 256, 2000);
    if (snapshot.ok()) {
        const auto* data = static_cast<const std::byte*>(snapshot.value().data());
        for (std::size_t index = 0; index < 256; ++index) {
            const auto expected = index >= 32 && index < 160 ? std::byte {0x7a} : std::byte {0x11};
            passed = passed && data[index] == expected;
        }
    } else {
        passed = false;
    }
    passed = passed && snapshot.ok() && snapshot.value().block_versions.size() == 4 &&
             host0.cached_replica_count() == 2 && host0.cached_replica_bytes() == 128;

    auto whole_write = host0.AcquireWriteRange(object.value(), 0, 128, 2000,
                                                cxloom::loommem::WriteAtomicity::kWholeRange);
    if (whole_write.ok())
        std::fill(whole_write.value().storage->begin(), whole_write.value().storage->end(), std::byte {0x33});
    passed = passed && whole_write.ok() && host0.ReleaseWriteBuffer(whole_write.value()).ok();
    auto* extent_allocator = dynamic_cast<cxloom::loommem::SharedExtentAllocator*>(&host0.allocator());
    const auto descriptor = extent_allocator == nullptr
                                ? cxloom::Result<cxloom::loommem::AllocationDescriptor*>(
                                      cxloom::Status::Internal("missing extent allocator"))
                                : extent_allocator->MutableDescriptor(object.value());
    passed = passed && descriptor.ok() &&
             (descriptor.value()->range_commit_epoch.load(std::memory_order_acquire) & 1U) == 0;
    if (descriptor.ok())
        descriptor.value()->range_commit_epoch.fetch_add(1, std::memory_order_acq_rel);
    auto blocked_reader = std::async(std::launch::async, [&] {
        return host1.AcquireReadRange(object.value(), 0, 128, 2000,
                                      cxloom::loommem::ReadConsistency::kWholeRange);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    passed = passed && blocked_reader.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    if (descriptor.ok())
        descriptor.value()->range_commit_epoch.fetch_add(1, std::memory_order_release);
    const auto whole_snapshot = blocked_reader.get();
    passed = passed && whole_snapshot.ok();
    if (whole_snapshot.ok()) {
        const auto* data = static_cast<const std::byte*>(whole_snapshot.value().data());
        passed = passed && std::all_of(data, data + 128, [](std::byte value) { return value == std::byte {0x33}; });
    }

    // Reclamation is object-wide. First return every block token to the
    // allocation owner, retain an immutable old-allocation snapshot, then
    // release data and sidecar extents back to their independent pools.
    cxloom::loommem::TokenLease stale_lease;
    for (std::uint64_t block = 0; block < 4; ++block) {
        const auto request = host0.RequestWriteToken(object.value(), block);
        const auto lease = request.ok() ? host0.WaitForWriteToken(request.value(), 2000)
                                        : cxloom::Result<cxloom::loommem::TokenLease>(request.status());
        passed = passed && lease.ok();
        if (lease.ok()) {
            if (block == 0) {
                stale_lease = lease.value();
                passed = passed && !host0.FreeShared(object.value()).ok();
            }
            passed = host0.ReleaseWriteToken(lease.value()).ok() && passed;
        }
    }
    const auto retained = host0.AcquireReadRange(object.value(), 0, 64, 2000);
    auto remote_reference = host1.AcquireObjectReference(object.value());
    passed = passed && remote_reference.ok() && !host0.FreeShared(object.value()).ok() &&
             host0.DescribeSharedAllocation(object.value()).ok();
    if (remote_reference.ok())
        remote_reference.value().guard.reset();
    const auto old_allocation_id = info.ok() ? info.value().allocation_id : 0;
    passed = passed && retained.ok() && host0.FreeShared(object.value()).ok() &&
             !host0.DescribeSharedAllocation(object.value()).ok();
    const auto reused = host0.AllocateShared(options);
    const auto reused_info = reused.ok() ? host0.DescribeSharedAllocation(reused.value())
                                         : cxloom::Result<cxloom::loommem::AllocationInfo>(reused.status());
    passed = passed && reused.ok() && reused.value().offset == object.value().offset && reused_info.ok() &&
             reused_info.value().allocation_id == old_allocation_id + 1 && retained.value().bytes() == 64 &&
             static_cast<const std::byte*>(retained.value().data())[0] == std::byte {0x33} &&
             !host0.ReleaseWriteToken(stale_lease).ok();

    // Retirement prevents new acquires but an already referenced writer can
    // complete, drop the final host reference, and let FreeShared finish.
    const auto retiring_object = host0.AllocateShared(options);
    auto retiring_write = retiring_object.ok()
                              ? host1.AcquireWriteRange(retiring_object.value(), 64, 64, 2000)
                              : cxloom::Result<cxloom::loommem::WriteBuffer>(retiring_object.status());
    auto retirement = std::async(std::launch::async, [&] {
        return retiring_object.ok() ? host0.FreeShared(retiring_object.value())
                                    : retiring_object.status();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool retire_is_hidden = retiring_object.ok() &&
                                  !host0.DescribeSharedAllocation(retiring_object.value()).ok();
    const auto release_retiring_write = retiring_write.ok()
                                            ? host1.ReleaseWriteBuffer(retiring_write.value())
                                            : retiring_write.status();
    const auto retirement_status = retirement.get();
    const bool retire_is_gone = retiring_object.ok() &&
                                !host0.DescribeSharedAllocation(retiring_object.value()).ok();
    passed = passed && retiring_write.ok() && retire_is_hidden && release_retiring_write.ok() &&
             retirement_status.ok() && retire_is_gone;

    const auto stop1 = host1.StopQueuePoller();
    const auto stop0 = host0.StopQueuePoller();
    host1.Finalize();
    host0.Finalize();
    std::remove(path);
    return passed && stop1.ok() && stop0.ok() ? 0 : 1;
}
