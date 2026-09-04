#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <iterator>
#include <thread>
#include <unistd.h>
#include <vector>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

namespace {

struct Record {
    std::uint64_t sequence;
    std::uint64_t writer;
    std::uint64_t checksum;
    std::uint64_t payload[29];
};

Record MakeRecord(std::uint64_t sequence, std::uint64_t writer) {
    Record record {};
    record.sequence = sequence;
    record.writer = writer;
    record.checksum = sequence ^ writer;
    for (std::size_t index = 0; index < 29; ++index) {
        record.payload[index] = sequence * 131 + writer * 17 + index;
        record.checksum ^= record.payload[index];
    }
    return record;
}

bool Valid(const cxloom::loommem::ReadSnapshot& snapshot, std::uint64_t sequence, std::uint64_t writer) {
    if (snapshot.bytes() != sizeof(Record) || snapshot.data() == nullptr)
        return false;
    const auto& record = *static_cast<const Record*>(snapshot.data());
    const auto expected = MakeRecord(sequence, writer);
    return record.sequence == expected.sequence && record.writer == expected.writer &&
           record.checksum == expected.checksum &&
           std::equal(std::begin(record.payload), std::end(record.payload), std::begin(expected.payload));
}

bool Store(cxloom::loommem::WriteBuffer& write, const Record& record) {
    if (write.bytes() != sizeof(record) || write.data() == nullptr)
        return false;
    *static_cast<Record*>(write.data()) = record;
    return true;
}

}  // namespace

int main() {
    char path[] = "/tmp/cxloom-coherence-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);

    constexpr std::size_t kRegionBytes = 192ULL << 20;
    cxloom::CxloomConfig config;
    config.host_count = 3;
    config.shared_region_bytes = kRegionBytes;
    config.shared_region_path = path;
    config.bootstrap_owner = true;
    config.create_region_file = true;
    config.queue_capacity_entries = 64;
    config.replica_cache_capacity_entries = 1;
    config.replica_cache_capacity_bytes = sizeof(Record);

    cxloom::loommem::LoomMemRuntime host0(config);
    if (!host0.Initialize().ok()) {
        std::remove(path);
        return 1;
    }
    config.bootstrap_owner = false;
    config.create_region_file = false;
    config.local_host_id = 1;
    cxloom::loommem::LoomMemRuntime host1(config);
    config.local_host_id = 2;
    cxloom::loommem::LoomMemRuntime host2(config);
    if (!host1.Initialize().ok() || !host2.Initialize().ok()) {
        host2.Finalize();
        host1.Finalize();
        host0.Finalize();
        std::remove(path);
        return 1;
    }

    const auto object = host0.AllocateShared(sizeof(Record), alignof(Record));
    const bool started = object.ok() && host0.StartQueuePoller().ok() && host1.StartQueuePoller().ok() &&
                         host2.StartQueuePoller().ok();
    if (!started) {
        host2.Finalize();
        host1.Finalize();
        host0.Finalize();
        std::remove(path);
        return 1;
    }

    auto first_write = host0.AcquireWriteBuffer(object.value(), 2000);
    bool passed = first_write.ok() && Store(first_write.value(), MakeRecord(1, 0)) &&
                  host0.ReleaseWriteBuffer(first_write.value()).ok();

    auto reader1 = std::async(std::launch::async, [&] { return host1.AcquireReadSnapshot(object.value(), 2000); });
    auto reader2 = std::async(std::launch::async, [&] { return host2.AcquireReadSnapshot(object.value(), 2000); });
    const auto first_read1 = reader1.get();
    const auto first_read2 = reader2.get();
    passed = passed && first_read1.ok() && first_read2.ok() && first_read1.value().version == 1 &&
             first_read2.value().version == 1 && Valid(first_read1.value(), 1, 0) && Valid(first_read2.value(), 1, 0);

    auto second_write = host1.AcquireWriteBuffer(object.value(), 2000);
    passed = passed && second_write.ok() && Store(second_write.value(), MakeRecord(2, 1));
    // A buffered writer only owns a private copy until release. Readers should
    // continue to see the last committed version while that token is held.
    auto concurrent_reader = host2.AcquireReadSnapshot(object.value(), 2000);
    passed = passed && concurrent_reader.ok() && concurrent_reader.value().version == 1 &&
             Valid(concurrent_reader.value(), 1, 0);
    passed = passed && host1.ReleaseWriteBuffer(second_write.value()).ok();
    const auto refreshed = host2.AcquireReadSnapshot(object.value(), 2000);
    passed = passed && refreshed.ok() && refreshed.value().version == 2 && Valid(refreshed.value(), 2, 1) &&
             Valid(first_read2.value(), 1, 0);

    std::vector<std::future<cxloom::Result<cxloom::loommem::ReadSnapshot>>> readers;
    for (std::size_t index = 0; index < 8; ++index)
        readers.push_back(
            std::async(std::launch::async, [&] { return host2.AcquireReadSnapshot(object.value(), 2000); }));
    for (auto& reader : readers) {
        const auto snapshot = reader.get();
        passed = passed && snapshot.ok() && snapshot.value().version == 2 && Valid(snapshot.value(), 2, 1) &&
                 snapshot.value().storage == refreshed.value().storage;
    }

    const auto host0_refreshed = host0.AcquireReadSnapshot(object.value(), 2000);
    passed = passed && host0_refreshed.ok() && host0_refreshed.value().version == 2 &&
             Valid(host0_refreshed.value(), 2, 1);

    auto held_write = host0.AcquireWriteBuffer(object.value(), 2000);
    const auto abandoned_write = host1.AcquireWriteBuffer(object.value(), 10);
    passed = passed && held_write.ok() && !abandoned_write.ok() &&
             abandoned_write.status().code() == cxloom::StatusCode::kUnavailable &&
             host0.ReleaseWriteBuffer(held_write.value()).ok();
    auto write_after_cancel = host2.AcquireWriteBuffer(object.value(), 2000);
    passed = passed && write_after_cancel.ok() && write_after_cancel.value().lease.version == 3 &&
             host2.ReleaseWriteBuffer(write_after_cancel.value()).ok();
    auto final_write = host0.AcquireWriteBuffer(object.value(), 2000);
    passed = passed && final_write.ok() && final_write.value().lease.version == 4 &&
             host0.ReleaseWriteBuffer(final_write.value()).ok();

    // LRU eviction drops only the runtime's cache reference. A snapshot held
    // by the application remains valid, while reacquiring the evicted object
    // transparently refreshes it from CXL.
    const auto retained_snapshot = host2.AcquireReadSnapshot(object.value(), 2000);
    const auto second_object = host0.AllocateShared(sizeof(Record), alignof(Record));
    auto other_write = second_object.ok() ? host0.AcquireWriteBuffer(second_object.value(), 2000)
                                          : cxloom::Result<cxloom::loommem::WriteBuffer>(second_object.status());
    passed = passed && retained_snapshot.ok() && other_write.ok() &&
             Store(other_write.value(), MakeRecord(1, 0)) && host0.ReleaseWriteBuffer(other_write.value()).ok();
    const auto other_snapshot = second_object.ok()
                                    ? host2.AcquireReadSnapshot(second_object.value(), 2000)
                                    : cxloom::Result<cxloom::loommem::ReadSnapshot>(second_object.status());
    passed = passed && other_snapshot.ok() && host2.cached_replica_count() == 1 &&
             host2.cached_replica_bytes() == sizeof(Record) && retained_snapshot.value().version == 5 &&
             Valid(retained_snapshot.value(), 2, 1);
    const auto reloaded_snapshot = host2.AcquireReadSnapshot(object.value(), 2000);
    passed = passed && reloaded_snapshot.ok() && reloaded_snapshot.value().version == 5 &&
             Valid(reloaded_snapshot.value(), 2, 1) && host2.cached_replica_count() == 1;

    const auto stop2 = host2.StopQueuePoller();
    const auto stop1 = host1.StopQueuePoller();
    const auto stop0 = host0.StopQueuePoller();
    host2.Finalize();
    host1.Finalize();
    host0.Finalize();
    std::remove(path);
    return passed && stop2.ok() && stop1.ok() && stop0.ok() ? 0 : 1;
}
