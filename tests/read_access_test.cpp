#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>

#include "cxloom/loommem/runtime.h"
#include "shared_region_fixture.h"

using namespace cxloom;
using namespace cxloom::loommem;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "read access line %d: %s\n", __LINE__, #condition); std::abort(); } } while (false)

template <class Predicate> bool Eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

static void Store(LoomMemRuntime& host, GlobalPointer object, std::uint64_t offset,
                  std::uint64_t bytes, unsigned char value) {
    auto write = host.AcquireWriteRange(object, offset, bytes, 2000);
    CHECK(write.ok());
    std::fill(write.value().storage->begin(), write.value().storage->end(), std::byte{value});
    CHECK(host.ReleaseWriteBuffer(write.value()).ok());
}

static ReadSnapshot Read(LoomMemRuntime& host, GlobalPointer object, std::uint64_t offset,
                         std::uint64_t bytes, unsigned char value) {
    const auto result = host.AcquireReadRange(object, offset, bytes, 2000);
    CHECK(result.ok());
    const auto* data = static_cast<const std::byte*>(result.value().data());
    CHECK(std::all_of(data, data + bytes, [=](std::byte b) { return b == std::byte{value}; }));
    return result.value();
}

static void BoundaryAndHolders() {
    CxloomConfig config;
    config.host_count = 2;
    config.shared_region_bytes = 192ULL << 20;
    config.bootstrap_timeout_ms = 2000;
    config.replica_cache_capacity_entries = 4;
    config.replica_cache_capacity_bytes = 256;
    SharedRegionFixture region(config);
    LoomMemRuntime owner(config);
    CHECK(owner.Initialize().ok());
    config.local_host_id = 1;
    config.bootstrap_owner = config.create_region_file = false;
    LoomMemRuntime reader(config);
    CHECK(reader.Initialize().ok());
    CHECK(owner.StartQueuePoller().ok() && reader.StartQueuePoller().ok());
    const auto allocation = owner.AllocateShared(AllocationOptions{192, 64, 64});
    CHECK(allocation.ok());
    const auto object = allocation.value();
    const auto found = owner.allocator().MutableDescriptor(object);
    CHECK(found.ok());
    auto* descriptor = found.value();
    Store(owner, object, 0, 192, 0x11);
    CHECK(descriptor->replica_reference_count() == 0);
    const auto first = Read(reader, object, 0, 64, 0x11);
    CHECK(descriptor->replica_reference_count() == 1);
    CHECK(descriptor->active_operations[1].load() == 0);
    Read(reader, object, 64, 64, 0x11); // Extend partial coverage, same host membership.
    CHECK(descriptor->replica_reference_count() == 1);
    Read(owner, object, 0, 64, 0x11);
    CHECK(descriptor->replica_reference_count() == 2);
    CHECK(owner.InvalidateReadCache(object).ok());
    CHECK(descriptor->replica_reference_count() == 1);

    Store(owner, object, 0, 64, 0x22);
    CHECK(Read(reader, object, 0, 64, 0x11).storage == first.storage);
    // current is valid even when shared metadata is inaccessible/odd.
    auto* block = owner.allocator().MutableCoherenceBlock(object, 0).value();
    const auto epoch = block->writeback_epoch.load();
    block->writeback_epoch.store(epoch + 1);
    descriptor->access_lock.store(1);
    auto cached = std::async(std::launch::async, [&] { return reader.AcquireReadRange(object, 0, 64, 100); });
    const bool cached_ready = cached.wait_for(200ms) == std::future_status::ready;
    descriptor->access_lock.store(0);
    block->writeback_epoch.store(epoch);
    CHECK(cached_ready && cached.get().ok());

    CHECK(reader.SynchronizeAcquire().ok());
    CHECK(reader.cached_replica_count() == 2); // old index retained.
    const auto changed = Read(reader, object, 0, 64, 0x22);
    CHECK(changed.storage != first.storage);
    CHECK(descriptor->replica_reference_count() == 1);
    // Validating block 0 must not validate an unrequested old block 1.
    Store(owner, object, 64, 64, 0x33);
    const auto second = Read(reader, object, 64, 64, 0x33);
    CHECK(reader.SynchronizeAcquire().ok());
    CHECK(Read(reader, object, 64, 64, 0x33).storage == second.storage); // unchanged promotion.
    CHECK(descriptor->replica_reference_count() == 1);
    CHECK(reader.SynchronizeAcquire().ok()); // discard untouched old block 0.
    CHECK(reader.cached_replica_count() == 1);
    CHECK(reader.SynchronizeAcquire().ok()); // discard the last old block.
    CHECK(reader.cached_replica_count() == 0 && descriptor->replica_reference_count() == 0);
    CHECK(static_cast<const std::byte*>(first.data())[0] == std::byte{0x11});

    // Concurrent first fills of one object register exactly one holder.
    std::vector<std::future<Result<ReadSnapshot>>> reads;
    for (unsigned i = 0; i < 8; ++i)
        reads.push_back(std::async(std::launch::async, [&] { return reader.AcquireReadRange(object, 0, 64, 2000); }));
    std::shared_ptr<const std::vector<std::byte>> storage;
    for (auto& read : reads) {
        const auto result = read.get();
        CHECK(result.ok());
        if (storage) CHECK(storage == result.value().storage);
        storage = result.value().storage;
    }
    CHECK(descriptor->replica_reference_count() == 1);
    CHECK(descriptor->active_operations[1].load() == 0);
    // Local committed writes invalidate cached blocks in either index.
    CHECK(reader.SynchronizeAcquire().ok());
    Store(reader, object, 0, 64, 0x44);
    Read(reader, object, 0, 64, 0x44);
    CHECK(reader.InvalidateReadCache(object).ok());
    CHECK(descriptor->replica_reference_count() == 0);

    // The boundary cannot finish while a pre-boundary fill is in progress.
    auto* busy = owner.allocator().MutableCoherenceBlock(object, 2).value();
    const auto busy_epoch = busy->writeback_epoch.load();
    busy->writeback_epoch.store(busy_epoch + 1);
    auto filling = std::async(std::launch::async, [&] { return reader.AcquireReadRange(object, 128, 64, 2000); });
    CHECK(Eventually([&] { return descriptor->active_operations[1].load() != 0; }));
    std::atomic<bool> boundary_started {false};
    auto boundary = std::async(std::launch::async, [&] {
        boundary_started = true;
        return reader.SynchronizeAcquire();
    });
    CHECK(Eventually([&] { return boundary_started.load(); }));
    CHECK(boundary.wait_for(20ms) == std::future_status::timeout);
    busy->writeback_epoch.store(busy_epoch);
    CHECK(filling.get().ok() && boundary.get().ok());
    Store(owner, object, 128, 64, 0x55);
    Read(reader, object, 128, 64, 0x55); // Must validate the pre-boundary fill.
    CHECK(owner.FreeShared(object).ok());
    CHECK(reader.cached_replica_count() == 0);
    // No allocation identity: an old address can resolve the new allocation.
    const auto reused = owner.AllocateShared(AllocationOptions{192, 64, 64});
    CHECK(reused.ok() && reused.value() == object);
    Store(owner, reused.value(), 0, 192, 0x66);
    Read(reader, object, 0, 192, 0x66);
    CHECK(owner.FreeShared(reused.value()).ok());
    CHECK(reader.Finalize().ok() && owner.Finalize().ok());
}

static void IndependentObjectsAndEviction() {
    CxloomConfig config;
    config.host_count = 1;
    config.shared_region_bytes = 192ULL << 20;
    config.replica_cache_capacity_entries = 2;
    config.replica_cache_capacity_bytes = 128;
    SharedRegionFixture region(config);
    LoomMemRuntime host(config);
    CHECK(host.Initialize().ok());
    const auto a = host.AllocateShared(AllocationOptions{192, 64, 64});
    const auto b = host.AllocateShared(64, 64);
    CHECK(a.ok() && b.ok());
    Store(host, a.value(), 0, 192, 0x11);
    Store(host, b.value(), 0, 64, 0x22);
    auto* ad = host.allocator().MutableDescriptor(a.value()).value();
    auto* bd = host.allocator().MutableDescriptor(b.value()).value();
    ad->access_lock.store(1);
    auto blocked = std::async(std::launch::async, [&] { return host.AcquireReadRange(a.value(), 0, 64, 2000); });
    auto independent = std::async(std::launch::async, [&] { return host.AcquireReadSnapshot(b.value(), 100); });
    const bool independent_ready = independent.wait_for(200ms) == std::future_status::ready;
    ad->access_lock.store(0);
    CHECK(independent_ready && independent.get().ok() && blocked.get().ok());
    CHECK(host.InvalidateReadCache(a.value()).ok() && host.InvalidateReadCache(b.value()).ok());

    // A cold read and Describe require no global allocator lock.
    auto* header = reinterpret_cast<AllocatorHeader*>(static_cast<std::byte*>(host.region_mapper().base()) +
                                                       host.layout().allocator.offset);
    header->extent_lock.store(1);
    auto no_global = std::async(std::launch::async, [&] {
        return host.allocator().Describe(b.value()).ok() && host.AcquireReadSnapshot(b.value(), 100).ok();
    });
    const bool no_global_ready = no_global.wait_for(200ms) == std::future_status::ready;
    header->extent_lock.store(0);
    CHECK(no_global_ready && no_global.get());
    CHECK(host.InvalidateReadCache(b.value()).ok());
    Read(host, a.value(), 0, 64, 0x11);
    Read(host, a.value(), 64, 64, 0x11);
    CHECK(host.SynchronizeAcquire().ok());
    Read(host, b.value(), 0, 64, 0x22); // Evict one old A block, preserve A membership.
    CHECK(ad->replica_reference_count() == 1 && bd->replica_reference_count() == 1);
    CHECK(host.cached_replica_count() == 2 && host.cached_replica_bytes() == 128);
    CHECK(host.InvalidateReadCache(b.value()).ok());
    CHECK(bd->replica_reference_count() == 0 && ad->replica_reference_count() == 1);
    CHECK(host.SynchronizeAcquire().ok()); // Drop last old A block.
    CHECK(ad->replica_reference_count() == 0);

    // Raw write release must unblock a local fill waiting on its odd epoch.
    auto request = host.RequestWriteToken(b.value());
    CHECK(request.ok());
    auto lease = host.WaitForWriteToken(request.value(), 2000);
    CHECK(lease.ok());
    auto fill = std::async(std::launch::async, [&] { return host.AcquireReadSnapshot(b.value(), 2000); });
    CHECK(Eventually([&] { return bd->active_operations[0].load() != 0; }));
    CHECK(host.ReleaseWriteToken(lease.value()).ok());
    CHECK(fill.get().ok());
    CHECK(host.FreeShared(a.value()).ok() && host.FreeShared(b.value()).ok());
    CHECK(host.Finalize().ok());
}

static void DiscoveryDuringReuse() {
    CxloomConfig config;
    config.host_count = 1;
    config.shared_region_bytes = 192ULL << 20;
    config.bootstrap_timeout_ms = 2000;
    SharedRegionFixture region(config);
    LoomMemRuntime host(config);
    CHECK(host.Initialize().ok());
    const auto allocation = host.AllocateShared(AllocationOptions{64, 64, 64});
    CHECK(allocation.ok());
    const auto address = allocation.value();
    std::atomic<bool> done {false};
    std::vector<std::thread> lookups;
    for (int i = 0; i < 4; ++i) lookups.emplace_back([&] {
        while (!done.load()) {
            const auto description = host.allocator().Describe(address);
            if (description.ok()) {
                CHECK(description.value().bytes == 64 || description.value().bytes == 256);
                CHECK(description.value().coherence_block_count == description.value().bytes / 64);
            }
            const auto operation = host.allocator().AcquireReference(address, 0);
            if (operation.ok()) {
                CHECK(operation.value()->bytes == 64 || operation.value()->bytes == 256);
                CHECK(host.allocator().ReleaseReference(operation.value(), 0).ok());
            }
        }
    });
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(host.FreeShared(address).ok());
        const auto next = host.AllocateShared(AllocationOptions{i % 2 ? 64U : 256U, 64, 64});
        CHECK(next.ok() && next.value() == address);
    }
    done = true;
    for (auto& lookup : lookups) lookup.join();
    CHECK(host.FreeShared(address).ok());
    CHECK(host.Finalize().ok());
}

int main() {
    BoundaryAndHolders();
    IndependentObjectsAndEviction();
    DiscoveryDuringReuse();
}
