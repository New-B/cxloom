#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <vector>

#include "cxloom/loommem/runtime.h"
#include "shared_region_fixture.h"

using namespace cxloom;
using namespace cxloom::loommem;
#define CHECK(x) do { if (!(x)) { std::cerr << "retirement test line " << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)

static void Write(LoomMemRuntime& host, GlobalPointer object, unsigned char value) {
    auto write = host.AcquireWriteBuffer(object, 2000);
    CHECK(write.ok());
    std::memset(write.value().data(), value, write.value().bytes());
    CHECK(host.ReleaseWriteBuffer(write.value()).ok());
}

static ReadSnapshot Read(LoomMemRuntime& host, GlobalPointer object, unsigned char value) {
    auto read = host.AcquireReadSnapshot(object, 2000);
    CHECK(read.ok());
    const auto* bytes = static_cast<const unsigned char*>(read.value().data());
    CHECK(std::all_of(bytes, bytes + read.value().bytes(), [value](unsigned char byte) { return byte == value; }));
    return read.value();
}

int main() {
    CxloomConfig config;
    config.host_count = 3;
    config.shared_region_bytes = 192ULL << 20;
    config.coherence_granule_bytes = 64;
    config.queue_capacity_entries = 2;
    config.bootstrap_timeout_ms = 150;
    SharedRegionFixture region(config);
    LoomMemRuntime owner(config);
    CHECK(owner.Initialize().ok());
    config.local_host_id = 1;
    config.bootstrap_owner = false;
    config.create_region_file = false;
    LoomMemRuntime writer(config);
    CHECK(writer.Initialize().ok());
    config.local_host_id = 2;
    LoomMemRuntime reader(config);
    CHECK(reader.Initialize().ok());

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, resume = false;
    CHECK(owner.StartQueuePoller().ok());
    CHECK(writer.StartQueuePoller([&](QueueEnvelope) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return resume; });
        return Status::Ok();
    }).ok());
    CHECK(reader.StartQueuePoller().ok());

    const auto allocation = owner.AllocateShared(512, 64);
    CHECK(allocation.ok());
    const auto object = allocation.value();
    Write(writer, object, 0x11);
    Read(owner, object, 0x11);
    Read(writer, object, 0x11);
    const auto retained = Read(reader, object, 0x11);
    const auto descriptor = owner.allocator().MutableDescriptor(object);
    CHECK(descriptor.ok());
    const auto extent_start = descriptor.value()->data_extent_offset;
    const auto extent_end = extent_start + descriptor.value()->data_extent_bytes;
    const auto sidecar = descriptor.value()->coherence_metadata_offset;

    // A popped message has not completed until its handler returns. Pause a
    // participant there, then saturate its inbound token queue and sender outbox.
    QueueEnvelope blocker;
    blocker.header = {MessageKind::kLoadUpdate, 0, 1, 0};
    CHECK(owner.GetQueue(0, 1).value()->Push(blocker).ok());
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    std::vector<TokenRequestHandle> pending;
    for (int i = 0; i < 8; ++i) {
        auto request = reader.RequestWriteToken(object, 0);
        CHECK(request.ok());
        pending.push_back(request.value());
    }
    CHECK(owner.FreeShared(object).code() == StatusCode::kUnavailable);
    const auto transaction = owner.allocator().Retirement();
    CHECK(transaction.phase == RetirementPhase::kClosing);
    CHECK(!owner.DescribeSharedAllocation(object).ok());
    // A valid DRAM hit remains usable during Closing; explicit invalidation
    // forces CXL admission, which must reject the retiring object.
    CHECK(reader.AcquireReadSnapshot(object, 10).ok());
    CHECK(reader.InvalidateReadCache(object).ok());
    CHECK(!reader.AcquireReadSnapshot(object, 10).ok());
    CHECK(!writer.AcquireWriteBuffer(object, 10).ok());
    CHECK(!reader.RequestWriteToken(object).ok());
    CHECK(!owner.allocator().Free(object).ok()); // No bypass of the completion certificate.
    for (const auto& request : pending) {
        const auto rejected = reader.WaitForWriteToken(request, 1000);
        CHECK(!rejected.ok() && rejected.status().code() != StatusCode::kUnavailable);
    }
    CHECK(reader.pending_token_request_count() == 0);
    const auto separate = owner.AllocateShared(512, 64);
    CHECK(separate.ok());
    const auto separate_descriptor = owner.allocator().MutableDescriptor(separate.value());
    CHECK(separate_descriptor.ok());
    const auto separate_start = separate_descriptor.value()->data_extent_offset;
    const auto separate_end = separate_start + separate_descriptor.value()->data_extent_bytes;
    CHECK(separate_end <= extent_start || separate_start >= extent_end);

    // Timeout is resumable; it never republishes the allocation as ALLOCATED.
    {
        std::lock_guard<std::mutex> lock(mutex);
        resume = true;
    }
    changed.notify_all();
    CHECK(owner.FreeShared(object).ok());
    CHECK(owner.allocator().Retirement().sequence == transaction.sequence);
    CHECK(owner.cached_replica_count() == 0);
    CHECK(writer.cached_replica_count() == 0);
    CHECK(reader.cached_replica_count() == 0);
    CHECK(static_cast<const unsigned char*>(retained.data())[0] == 0x11);

    // Reuse only part of the old allocation, and reuse its sidecar with the
    // same block version. Every host must load the new data, not its old cache.
    const auto smaller = owner.AllocateShared(128, 64);
    CHECK(smaller.ok() && smaller.value() == object);
    CHECK(owner.DescribeSharedAllocation(smaller.value()).value().coherence_metadata_offset == sidecar);
    Write(owner, smaller.value(), 0x77);
    auto fresh = Read(reader, smaller.value(), 0x77);
    CHECK(fresh.block_versions.front() == retained.block_versions.front());
    Read(writer, smaller.value(), 0x77);

    // An existing writer may finish, but no new operation is admitted. The
    // private buffer and its leases keep retirement from closing this host.
    auto active = writer.AcquireWriteBuffer(smaller.value(), 2000);
    CHECK(active.ok());
    std::memset(active.value().data(), 0x55, active.value().bytes());
    CHECK(owner.FreeShared(smaller.value()).code() == StatusCode::kUnavailable);
    CHECK(!owner.AcquireObjectReference(smaller.value()).ok());
    CHECK(writer.ReleaseWriteBuffer(active.value()).ok());
    CHECK(owner.FreeShared(smaller.value()).ok());
    CHECK(reader.cached_replica_count() == 0 && writer.cached_replica_count() == 0);

    // Delayed publication is also existing activity: the originating thread
    // can release its staged write while the object remains RETIRING.
    const auto staged_object = owner.AllocateShared(128, 64);
    CHECK(staged_object.ok());
    auto staged = writer.AcquireWriteBuffer(staged_object.value(), 2000);
    CHECK(staged.ok());
    CHECK(writer.StageWriteBuffer(&staged.value()).ok());
    CHECK(owner.FreeShared(staged_object.value()).code() == StatusCode::kUnavailable);
    CHECK(writer.SynchronizeRelease().ok());
    CHECK(owner.FreeShared(staged_object.value()).ok());

    // Reclaiming a larger range assembled from previously freed extents is
    // independent of all earlier allocation shapes.
    CHECK(owner.FreeShared(separate.value()).ok());
    const auto larger = owner.AllocateShared(2048, 64);
    CHECK(larger.ok() && larger.value() == object);
    Write(owner, larger.value(), 0x99);
    Read(reader, larger.value(), 0x99);
    CHECK(owner.FreeShared(larger.value()).ok());
    CHECK(owner.Finalize().ok());
    CHECK(writer.Finalize().ok());
    CHECK(reader.Finalize().ok());
    std::cout << "all-host retirement, callback drain, timeout resume and partial-overlap reuse passed\n";
}
