#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <utility>

#include "cxloom/loommem.h"
#include "shared_region_fixture.h"

using namespace cxloom;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "staged write API line %d: %s\n", __LINE__, #condition); std::abort(); } } while (false)

static void Fill(WriteView& view, unsigned char value) {
    CHECK(view.data() != nullptr);
    std::fill_n(static_cast<unsigned char*>(view.data()), view.size(), value);
}

static ReadView Read(Context& context, GPtr object, std::uint64_t offset,
                     std::uint64_t bytes, unsigned char value) {
    auto read = clReadRange(context, object, offset, bytes, 2000);
    CHECK(read.ok());
    const auto* data = static_cast<const unsigned char*>(read.value().data());
    CHECK(std::all_of(data, data + bytes, [=](auto byte) { return byte == value; }));
    return read.value();
}

int main() {
    CxloomConfig config;
    config.host_count = 2;
    config.shared_region_bytes = 192ULL << 20;
    config.queue_capacity_entries = 64;
    config.bootstrap_timeout_ms = 200;
    SharedRegionFixture region(config);
    auto owner = clInit(config);
    CHECK(owner.ok());
    config.local_host_id = 1;
    config.bootstrap_owner = config.create_region_file = false;
    auto peer = clInit(config);
    CHECK(peer.ok());
    auto object = clAlloc(*owner.value(), AllocOptions{192, 64, 64});
    CHECK(object.ok());
    auto initial = clWrite(*owner.value(), object.value(), 2000);
    CHECK(initial.ok());
    Fill(initial.value(), 0x11);
    CHECK(initial.value().Commit().ok());
    CHECK(!initial.value().Stage().ok());
    WriteView empty;
    CHECK(!empty.Stage().ok());

    // Multiple writes are transferred, including a partial range crossing blocks.
    // Destruction of the original view must neither abort nor publish the buffer.
    {
        auto range = clWriteRange(*owner.value(), object.value(), 32, 96, 2000);
        CHECK(range.ok());
        Fill(range.value(), 0x22);
        CHECK(range.value().Stage().ok());
        const WriteView& inactive = range.value();
        CHECK(range.value().data() == nullptr && inactive.data() == nullptr && inactive.size() == 0);
        CHECK(!range.value().Stage().ok() && !range.value().Commit().ok() && !range.value().Abort().ok());
        auto last = clWriteRange(*owner.value(), object.value(), 128, 64, 2000);
        CHECK(last.ok());
        Fill(last.value(), 0x33);
        CHECK(last.value().Stage().ok());
    }
    const auto retained = Read(*peer.value(), object.value(), 0, 192, 0x11);
    // Acquire is not a release and cannot publish staged writes.
    CHECK(clSynchronizeAcquire(*owner.value()).ok());
    Read(*owner.value(), object.value(), 0, 192, 0x11);
    // Tokens remain held until publication; a remote competing writer times out.
    auto blocked = clWriteRange(*peer.value(), object.value(), 0, 64, 10);
    CHECK(!blocked.ok() && blocked.status().code() == StatusCode::kUnavailable);
    auto* before_destroy = owner.value().get();
    CHECK(clDestroy(owner.value()).code() == StatusCode::kFailedPrecondition);
    CHECK(owner.value().get() == before_destroy);
    CHECK(clSynchronizeRelease(*owner.value()).ok());
    // Remote current replicas remain valid until the matching acquire boundary.
    Read(*peer.value(), object.value(), 0, 192, 0x11);
    CHECK(clSynchronizeAcquire(*peer.value()).ok());
    Read(*peer.value(), object.value(), 0, 32, 0x11);
    Read(*peer.value(), object.value(), 32, 96, 0x22);
    Read(*peer.value(), object.value(), 128, 64, 0x33);
    CHECK(static_cast<const unsigned char*>(retained.data())[32] == 0x11);
    // A second empty release succeeds, and tokens are available again.
    CHECK(clSynchronizeRelease(*owner.value()).ok());
    auto acquired = clWrite(*peer.value(), object.value(), 2000);
    CHECK(acquired.ok() && acquired.value().Abort().ok());
    CHECK(!acquired.value().Stage().ok());

    // Ownership may move to another native thread. Stage binds to the staging
    // thread, not the thread that originally acquired the WriteView.
    auto transferred = clWrite(*owner.value(), object.value(), 2000);
    CHECK(transferred.ok());
    Fill(transferred.value(), 0x44);
    std::promise<void> staged, release;
    auto staged_ready = staged.get_future();
    auto release_ready = release.get_future();
    std::thread worker([view = std::move(transferred.value()), &staged, &release_ready,
                        context = owner.value().get()]() mutable {
        CHECK(view.Stage().ok());
        view = WriteView{}; // Moving over the staged wrapper must not abort it.
        staged.set_value();
        release_ready.wait();
        CHECK(clSynchronizeRelease(*context).ok());
    });
    staged_ready.wait();
    CHECK(!transferred.value().Stage().ok());
    CHECK(clSynchronizeRelease(*owner.value()).ok()); // Different thread: no publication.
    CHECK(clSynchronizeAcquire(*peer.value()).ok());
    Read(*peer.value(), object.value(), 32, 96, 0x22);
    Read(*peer.value(), object.value(), 128, 64, 0x33);
    release.set_value();
    worker.join();
    CHECK(clSynchronizeAcquire(*peer.value()).ok());
    Read(*peer.value(), object.value(), 0, 192, 0x44);

    // Retirement cannot reclaim staged storage. The staging thread can still
    // publish an admitted write, then the owner can finish the same retirement.
    auto retiring = clWrite(*peer.value(), object.value(), 2000);
    CHECK(retiring.ok());
    Fill(retiring.value(), 0x55);
    CHECK(retiring.value().Stage().ok());
    CHECK(clFree(*owner.value(), object.value()).code() == StatusCode::kUnavailable);
    CHECK(clSynchronizeRelease(*peer.value()).ok());
    CHECK(clFree(*owner.value(), object.value()).ok());
    CHECK(clDestroy(peer.value()).ok() && clDestroy(owner.value()).ok());
}
