#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "cxloom/loommem.h"
#include "shared_region_fixture.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

cxloom::CxloomConfig Config(unsigned hosts) {
    cxloom::CxloomConfig config;
    config.host_count = hosts;
    config.shared_region_bytes = 192ULL << 20;
    config.queue_capacity_entries = 64;
    config.bootstrap_timeout_ms = 200;
    return config;
}

void SingleHost() {
    auto config = Config(1);
    SharedRegionFixture region(config);
    auto context = cxloom::clInit(config);
    Check(context.ok(), "single-host init");
    auto object = cxloom::clAlloc(*context.value(), cxloom::AllocOptions{192, 64, 64});
    Check(object.ok(), "single-host allocation");
    auto write = cxloom::clWrite(*context.value(), object.value(), 1000);
    Check(write.ok(), "single-host write");
    std::fill_n(static_cast<unsigned char*>(write.value().data()), 192, 0x35);
    Check(write.value().Commit().ok(), "single-host commit");
    auto aborted = cxloom::clWriteRange(*context.value(), object.value(), 32, 96, 1000);
    Check(aborted.ok(), "single-host range write");
    std::fill_n(static_cast<unsigned char*>(aborted.value().data()), 96, 0x99);
    Check(aborted.value().Abort().ok(), "single-host abort");
    auto read = cxloom::clRead(*context.value(), object.value(), 1000);
    Check(read.ok(), "single-host read");
    const auto* data = static_cast<const unsigned char*>(read.value().data());
    Check(std::all_of(data, data + 192, [](auto byte) { return byte == 0x35; }), "abort preserves data");
    Check(cxloom::clFree(*context.value(), object.value()).ok(), "single-host free");
    Check(cxloom::clDestroy(context.value()).ok() && !context.value(), "single-host destroy");
    Check(data[0] == 0x35, "snapshot survives destroy");
}

void MoveAssignment() {
    auto config = Config(2);
    SharedRegionFixture region(config);
    auto owner = cxloom::clInit(config);
    config.local_host_id = 1;
    config.bootstrap_owner = false;
    config.create_region_file = false;
    auto peer = cxloom::clInit(config);
    Check(owner.ok() && peer.ok(), "move test init");
    auto object = cxloom::clAlloc(*owner.value(), cxloom::AllocOptions{256, 64, 64});
    Check(object.ok(), "move test allocation");
    auto seed = cxloom::clWrite(*owner.value(), object.value(), 1000);
    Check(seed.ok(), "move seed write");
    std::fill_n(static_cast<unsigned char*>(seed.value().data()), 256, 0x11);
    Check(seed.value().Commit().ok(), "move seed commit");
    auto left = cxloom::clWriteRange(*owner.value(), object.value(), 0, 128, 1000);
    auto right = cxloom::clWriteRange(*owner.value(), object.value(), 128, 128, 1000);
    Check(left.ok() && right.ok(), "move active ranges");
    std::fill_n(static_cast<unsigned char*>(left.value().data()), 128, 0x22);
    std::fill_n(static_cast<unsigned char*>(right.value().data()), 128, 0x33);
    left.value() = std::move(right.value());
    Check(right.value().data() == nullptr, "moved-from view empty");
    // Self-move must retain the active lease and its data.
    auto* self = &left.value();
    left.value() = std::move(*self);
    Check(left.value().data() != nullptr && left.value().Commit().ok(), "moved lease commits");
    auto reacquired = cxloom::clWriteRange(*peer.value(), object.value(), 0, 128, 1000);
    Check(reacquired.ok(), "overwritten leases released to peer");
    const auto* old = static_cast<const unsigned char*>(reacquired.value().data());
    Check(std::all_of(old, old + 128, [](auto byte) { return byte == 0x11; }), "overwritten changes aborted");
    // Assigning an empty view must also release an active destination.
    reacquired.value() = cxloom::WriteView{};
    auto read = cxloom::clRead(*owner.value(), object.value(), 1000);
    Check(read.ok(), "move result read");
    const auto* data = static_cast<const unsigned char*>(read.value().data());
    Check(std::all_of(data + 128, data + 256, [](auto byte) { return byte == 0x33; }), "moved changes published");
    Check(cxloom::clFree(*owner.value(), object.value()).ok(), "move object reclaimed");
    Check(cxloom::clDestroy(peer.value()).ok(), "move peer destroy");
    Check(cxloom::clDestroy(owner.value()).ok(), "move owner destroy");
}

void DestroyRetry() {
    auto config = Config(2);
    SharedRegionFixture region(config);
    auto owner = cxloom::clInit(config);
    config.local_host_id = 1;
    config.bootstrap_owner = false;
    config.create_region_file = false;
    auto peer = cxloom::clInit(config);
    Check(owner.ok() && peer.ok(), "retry test init");
    auto object = cxloom::clAlloc(*owner.value(), 64, 64);
    Check(object.ok(), "retry allocation");
    auto blocker = cxloom::clWrite(*peer.value(), object.value(), 1000);
    Check(blocker.ok(), "retry blocking writer");
    auto* peer_before = peer.value().get();
    Check(!cxloom::clDestroy(peer.value()).ok() && peer.value().get() == peer_before,
          "destroy retains active writer context");
    Check(cxloom::clFree(*owner.value(), object.value()).code() == cxloom::StatusCode::kUnavailable,
          "active remote writer delays retirement");
    auto* owner_before = owner.value().get();
    Check(cxloom::clDestroy(owner.value()).code() == cxloom::StatusCode::kFailedPrecondition &&
              owner.value().get() == owner_before, "destroy retains retiring context");
    // A remote token handoff proves the rejected destroy kept progress alive.
    auto other = cxloom::clAlloc(*owner.value(), 64, 64);
    Check(other.ok(), "retained context allocates");
    auto remote = cxloom::clWrite(*peer.value(), other.value(), 1000);
    Check(remote.ok(), "retained owner poller grants tokens");
    Check(remote.value().Abort().ok(), "remote abort");
    Check(blocker.value().Abort().ok(), "finish outstanding writer");
    Check(cxloom::clFree(*owner.value(), object.value()).ok(), "resume retirement");
    Check(cxloom::clFree(*owner.value(), other.value()).ok(), "reclaim other object");
    Check(cxloom::clDestroy(peer.value()).ok() && !peer.value(), "retry peer destroy");
    Check(cxloom::clDestroy(owner.value()).ok() && !owner.value(), "retry owner destroy");
}
}  // namespace

int main() {
    bool passed = true;
    for (auto test : {SingleHost, MoveAssignment, DestroyRetry}) {
        try { test(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "LoomMem lifecycle regression: %s\n", error.what());
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
