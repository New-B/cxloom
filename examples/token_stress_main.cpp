#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

namespace {

struct alignas(64) StressObject {
  std::atomic<std::uint64_t> ready{0};
  std::atomic<std::uint64_t> done{0};
  std::atomic<std::uint64_t> final_ready{0};
  std::uint64_t counter{0};
  std::uint64_t checksum{0};
  std::uint64_t last_host{UINT64_MAX};
};

std::uint64_t Parse(const char *value, std::uint64_t fallback) {
  if (value == nullptr || *value == '\0')
    return fallback;
  char *end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  return end != value && *end == '\0' ? parsed : fallback;
}

std::uint64_t Checksum(std::uint64_t counter, std::uint64_t last_host) {
  return (counter * UINT64_C(0x9e3779b97f4a7c15)) ^
         (last_host * UINT64_C(0xd6e8feb86659fd93));
}

bool WaitFor(std::atomic<std::uint64_t> &value, std::uint64_t target,
             std::uint64_t timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (value.load(std::memory_order_acquire) < target &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  return value.load(std::memory_order_acquire) >= target;
}

} // namespace

int main() {
  const char *path = std::getenv("CL_DAX_DEVICE");
  if (path == nullptr || *path == '\0') {
    std::cerr << "CL_DAX_DEVICE is required\n";
    return 2;
  }

  cxloom::CxloomConfig config;
  config.local_host_id =
      static_cast<cxloom::HostId>(Parse(std::getenv("CL_HOST_ID"), 0));
  config.host_count =
      static_cast<std::uint16_t>(Parse(std::getenv("CL_HOST_COUNT"), 0));
  config.shared_region_bytes =
      Parse(std::getenv("CL_SHARED_REGION_BYTES"), config.shared_region_bytes);
  config.queue_capacity_entries = Parse(std::getenv("CL_QUEUE_CAPACITY"), 0);
  config.bootstrap_timeout_ms =
      Parse(std::getenv("CL_TOKEN_TIMEOUT_MS"), 60000);
  config.shared_region_path = path;
  config.bootstrap_owner = Parse(std::getenv("CL_BOOTSTRAP_OWNER"), 0) != 0;
  const auto iterations = Parse(std::getenv("CL_TOKEN_ITERATIONS"), 10000);
  if (config.host_count < 2 || config.host_count > cxloom::kMaxHosts ||
      config.local_host_id >= config.host_count ||
      iterations == 0) {
    std::cerr << "token stress requires 2.." << cxloom::kMaxHosts
              << " hosts and non-zero iterations\n";
    return 2;
  }

  cxloom::loommem::LoomMemRuntime runtime(config);
  auto status = runtime.Initialize();
  if (!status.ok()) {
    std::cerr << "runtime initialization failed: " << status.message() << "\n";
    return 1;
  }
  status = runtime.PublishBootstrapProbe(config.local_host_id + 1);
  if (status.ok())
    status = runtime.WaitForAllHosts(config.bootstrap_timeout_ms);
  if (!status.ok()) {
    std::cerr << "host rendezvous failed: " << status.message() << "\n";
    return 1;
  }

  cxloom::GlobalPointer object_pointer;
  if (config.local_host_id == 0) {
    const auto allocation =
        runtime.AllocateShared(sizeof(StressObject), alignof(StressObject));
    if (!allocation.ok()) {
      std::cerr << "allocation failed: " << allocation.status().message() << "\n";
      return 1;
    }
    object_pointer = allocation.value();
    const auto local = runtime.ResolveLocal(object_pointer);
    if (!local.ok())
      return 1;
    new (local.value()) StressObject();
    status = runtime.PublishSharedObject(object_pointer, sizeof(StressObject));
    if (!status.ok()) {
      std::cerr << "object publication failed: " << status.message() << "\n";
      return 1;
    }
  } else {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config.bootstrap_timeout_ms);
    cxloom::Result<cxloom::loommem::PublishedSharedObject> published(
        cxloom::Status::Unavailable("not published"));
    while (!(published = runtime.ReadPublishedSharedObject(0)).ok() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    if (!published.ok() || published.value().bytes != sizeof(StressObject)) {
      std::cerr << "failed to discover stress object\n";
      return 1;
    }
    object_pointer = published.value().gptr;
  }

  const auto local = runtime.ResolveLocal(object_pointer);
  if (!local.ok())
    return 1;
  auto *object = static_cast<StressObject *>(local.value());
  if (!object->ready.is_lock_free() || !object->done.is_lock_free() ||
      !object->final_ready.is_lock_free()) {
    std::cerr << "shared coordination atomics are not lock-free\n";
    return 1;
  }

  cxloom::loommem::QueuePollerOptions options;
  options.batch_size = Parse(std::getenv("CL_TOKEN_BATCH_SIZE"), 32);
  status = runtime.StartQueuePoller({}, options);
  if (!status.ok()) {
    std::cerr << "poller startup failed: " << status.message() << "\n";
    return 1;
  }

  object->ready.fetch_add(1, std::memory_order_acq_rel);
  if (!WaitFor(object->ready, config.host_count, config.bootstrap_timeout_ms)) {
    std::cerr << "workload rendezvous timed out\n";
    return 1;
  }

  std::uint64_t errors = 0;
  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const auto request = runtime.RequestWriteToken(object_pointer);
    if (!request.ok()) {
      ++errors;
      break;
    }
    const auto lease =
        runtime.WaitForWriteToken(request.value(), config.bootstrap_timeout_ms);
    if (!lease.ok()) {
      std::cerr << "token wait failed at iteration " << iteration << ": "
                << lease.status().message() << "\n";
      ++errors;
      break;
    }
    if (object->counter != 0 &&
        object->checksum != Checksum(object->counter, object->last_host))
      ++errors;
    ++object->counter;
    object->last_host = config.local_host_id;
    object->checksum = Checksum(object->counter, object->last_host);
    status = runtime.ReleaseWriteToken(lease.value());
    if (!status.ok()) {
      std::cerr << "token release failed at iteration " << iteration << ": "
                << status.message() << "\n";
      ++errors;
      break;
    }
  }

  object->done.fetch_add(1, std::memory_order_acq_rel);
  if (!WaitFor(object->done, config.host_count, config.bootstrap_timeout_ms)) {
    std::cerr << "completion rendezvous timed out\n";
    ++errors;
  }

  if (config.local_host_id == 0 && errors == 0) {
    const auto request = runtime.RequestWriteToken(object_pointer);
    const auto lease =
        request.ok()
            ? runtime.WaitForWriteToken(request.value(), config.bootstrap_timeout_ms)
            : cxloom::Result<cxloom::loommem::TokenLease>(request.status());
    const auto expected = iterations * config.host_count;
    if (!lease.ok() || object->counter != expected ||
        object->checksum != Checksum(object->counter, object->last_host) ||
        lease.value().version != expected) {
      std::cerr << "final invariant failed: counter=" << object->counter
                << " expected=" << expected
                << " version=" << (lease.ok() ? lease.value().version : 0)
                << "\n";
      ++errors;
    }
    if (lease.ok() && !runtime.ReleaseWriteToken(lease.value()).ok())
      ++errors;
    object->final_ready.store(1, std::memory_order_release);
  } else if (!WaitFor(object->final_ready, 1, config.bootstrap_timeout_ms)) {
    std::cerr << "final validation timed out\n";
    ++errors;
  }

  const auto stop = runtime.StopQueuePoller();
  if (!stop.ok()) {
    std::cerr << "poller stopped with error: " << stop.message() << "\n";
    ++errors;
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  const auto *poller = runtime.queue_poller();
  std::cout << "host=" << config.local_host_id << " hosts=" << config.host_count
            << " iterations=" << iterations << " elapsed_ms=" << elapsed_ms
            << " poller_cpu=" << poller->bound_cpu()
            << " messages=" << poller->stats().messages
            << " errors=" << errors << "\n";
  runtime.Finalize();
  return errors == 0 ? 0 : 1;
}
