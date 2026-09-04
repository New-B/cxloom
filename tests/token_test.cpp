#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unistd.h>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

namespace {

bool WaitUntilPending(cxloom::loommem::LoomMemRuntime& runtime,
                      const cxloom::loommem::TokenRequestHandle& request) {
    const auto result = runtime.WaitForWriteToken(request, 10);
    return !result.ok() && result.status().code() == cxloom::StatusCode::kUnavailable;
}

}  // namespace

int main() {
    char path[] = "/tmp/cxloom-token-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);

    constexpr std::size_t kRegionBytes = 192ULL << 20;
    cxloom::CxloomConfig config;
    config.local_host_id = 0;
    config.host_count = 2;
    config.shared_region_bytes = kRegionBytes;
    config.shared_region_path = path;
    config.bootstrap_owner = true;
    config.create_region_file = true;
    config.queue_capacity_entries = 64;

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

    const auto object = host0.AllocateShared(sizeof(std::uint64_t), alignof(std::uint64_t));
    const bool started = object.ok() && host0.StartQueuePoller().ok() && host1.StartQueuePoller().ok();
    if (!started) {
        host1.Finalize();
        host0.Finalize();
        std::remove(path);
        return 1;
    }

    const auto local_request = host0.RequestWriteToken(object.value());
    const auto local_lease = local_request.ok() ? host0.WaitForWriteToken(local_request.value(), 1000)
                                                : cxloom::Result<cxloom::loommem::TokenLease>(
                                                      local_request.status());
    const auto remote_request = host1.RequestWriteToken(object.value());
    if (!local_lease.ok() || local_lease.value().version != 0 || local_lease.value().token_epoch != 1 ||
        !remote_request.ok() || !WaitUntilPending(host1, remote_request.value())) {
        host1.Finalize();
        host0.Finalize();
        std::remove(path);
        return 1;
    }

    const auto host0_data = host0.ResolveLocal(object.value());
    if (!host0_data.ok()) {
        host1.Finalize();
        host0.Finalize();
        std::remove(path);
        return 1;
    }
    *static_cast<std::uint64_t*>(host0_data.value()) = 41;

    const auto release0 = host0.ReleaseWriteToken(local_lease.value());
    const auto lease1 = remote_request.ok() ? host1.WaitForWriteToken(remote_request.value(), 2000)
                                            : cxloom::Result<cxloom::loommem::TokenLease>(
                                                  remote_request.status());
    const auto host1_data = host1.ResolveLocal(object.value());
    if (!release0.ok() || !lease1.ok() || lease1.value().version != 1 || lease1.value().token_epoch != 2 ||
        !host1_data.ok() || *static_cast<std::uint64_t*>(host1_data.value()) != 41 ||
        host0.ReleaseWriteToken(local_lease.value()).ok()) {
        host1.Finalize();
        host0.Finalize();
        std::remove(path);
        return 1;
    }

    const auto reverse_request = host0.RequestWriteToken(object.value());
    if (!reverse_request.ok() || !WaitUntilPending(host0, reverse_request.value())) {
        host1.Finalize();
        host0.Finalize();
        std::remove(path);
        return 1;
    }
    *static_cast<std::uint64_t*>(host1_data.value()) = 42;

    const auto release1 = host1.ReleaseWriteToken(lease1.value());
    const auto lease0_again = host0.WaitForWriteToken(reverse_request.value(), 2000);
    const bool passed = release1.ok() && lease0_again.ok() && lease0_again.value().version == 2 &&
                        lease0_again.value().token_epoch == 3 &&
                        *static_cast<std::uint64_t*>(host0_data.value()) == 42 &&
                        host0.ReleaseWriteToken(lease0_again.value()).ok();

    const auto stop1 = host1.StopQueuePoller();
    const auto stop0 = host0.StopQueuePoller();
    host1.Finalize();
    host0.Finalize();
    std::remove(path);
    return passed && stop1.ok() && stop0.ok() ? 0 : 1;
}
