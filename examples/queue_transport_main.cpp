#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

namespace {

struct Payload {
  std::uint64_t sequence{0};
  std::uint64_t pattern{0};
};

std::uint64_t Parse(const char *value, std::uint64_t fallback) {
  if (value == nullptr || *value == '\0')
    return fallback;
  char *end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  return end != value && *end == '\0' ? parsed : fallback;
}

std::uint64_t Pattern(cxloom::HostId source, cxloom::HostId destination,
                      std::uint64_t sequence) {
  return (sequence * UINT64_C(0x9e3779b97f4a7c15)) ^
         ((static_cast<std::uint64_t>(source) << 32) | destination);
}

cxloom::loommem::QueueEnvelope MakeMessage(cxloom::HostId source,
                                           cxloom::HostId destination,
                                           std::uint64_t sequence) {
  cxloom::loommem::QueueEnvelope message;
  message.header.kind = cxloom::MessageKind::kLoadUpdate;
  message.header.src_host = source;
  message.header.dst_host = destination;
  message.header.payload_bytes = sizeof(Payload);
  message.payload.resize(sizeof(Payload));
  const Payload payload{sequence, Pattern(source, destination, sequence)};
  std::memcpy(message.payload.data(), &payload, sizeof(payload));
  return message;
}

bool VerifyMessage(const cxloom::loommem::QueueEnvelope &message,
                   cxloom::HostId source, cxloom::HostId destination,
                   std::uint64_t sequence) {
  if (message.header.kind != cxloom::MessageKind::kLoadUpdate ||
      message.header.src_host != source ||
      message.header.dst_host != destination ||
      message.header.payload_bytes != sizeof(Payload) ||
      message.payload.size() != sizeof(Payload)) {
    return false;
  }
  Payload payload;
  std::memcpy(&payload, message.payload.data(), sizeof(payload));
  return payload.sequence == sequence &&
         payload.pattern == Pattern(source, destination, sequence);
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
      static_cast<std::uint16_t>(Parse(std::getenv("CL_HOST_COUNT"), 4));
  config.shared_region_bytes =
      Parse(std::getenv("CL_SHARED_REGION_BYTES"), config.shared_region_bytes);
  config.queue_capacity_entries = Parse(std::getenv("CL_QUEUE_CAPACITY"), 1024);
  config.bootstrap_timeout_ms =
      Parse(std::getenv("CL_QUEUE_TIMEOUT_MS"), 30000);
  config.shared_region_path = path;
  config.bootstrap_owner = Parse(std::getenv("CL_BOOTSTRAP_OWNER"), 0) != 0;
  const auto iterations = Parse(std::getenv("CL_QUEUE_ITERATIONS"), 100000);
  if (config.host_count != 4 || config.local_host_id >= config.host_count ||
      iterations == 0) {
    std::cerr << "queue transport litmus requires exactly four hosts and "
                 "non-zero iterations\n";
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

  std::array<std::uint64_t, cxloom::kMaxHosts> expected{};
  expected.fill(1);
  std::atomic<std::uint64_t> received{0};
  std::atomic<std::uint64_t> errors{0};

  cxloom::loommem::QueuePollerOptions poller_options;
  poller_options.batch_size = Parse(std::getenv("CL_QUEUE_BATCH_SIZE"), 32);
  const char *poller_cpu = std::getenv("CL_POLLER_CPU");
  if (poller_cpu != nullptr && *poller_cpu != '\0')
    poller_options.cpu_id = static_cast<int>(Parse(poller_cpu, 0));
  status = runtime.StartQueuePoller(
      [&](cxloom::loommem::QueueEnvelope message) {
        const auto source = message.header.src_host;
        if (source >= config.host_count || source == config.local_host_id ||
            !VerifyMessage(message, source, config.local_host_id,
                           expected[source])) {
          errors.fetch_add(1, std::memory_order_relaxed);
        } else {
          ++expected[source];
        }
        received.fetch_add(1, std::memory_order_release);
        return cxloom::Status::Ok();
      },
      poller_options);
  if (!status.ok()) {
    std::cerr << "queue poller startup failed: " << status.message() << "\n";
    return 1;
  }

  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t sequence = 1; sequence <= iterations; ++sequence) {
    for (cxloom::HostId destination = 0; destination < config.host_count;
         ++destination) {
      if (destination == config.local_host_id)
        continue;
      const auto queue = runtime.GetQueue(config.local_host_id, destination);
      if (!queue.ok()) {
        std::cerr << "outbound queue lookup failed\n";
        return 1;
      }
      while (true) {
        const auto push = queue.value()->Push(
            MakeMessage(config.local_host_id, destination, sequence));
        if (push.ok())
          break;
        if (push.code() != cxloom::StatusCode::kUnavailable) {
          std::cerr << "queue push failed: " << push.message() << "\n";
          return 1;
        }
        std::this_thread::yield();
      }
    }
  }

  const auto expected_messages = iterations * (config.host_count - 1);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(config.bootstrap_timeout_ms);
  while (received.load(std::memory_order_acquire) != expected_messages &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  if (received.load(std::memory_order_acquire) != expected_messages)
    errors.fetch_add(1, std::memory_order_relaxed);
  const auto poller_status = runtime.StopQueuePoller();
  if (!poller_status.ok())
    errors.fetch_add(1, std::memory_order_relaxed);

  const auto *poller = runtime.queue_poller();
  const auto stats = poller->stats();
  for (cxloom::HostId peer = 0; peer < config.host_count; ++peer) {
    if (peer == config.local_host_id)
      continue;
    const auto inbound = runtime.GetQueue(peer, config.local_host_id);
    if (!inbound.ok()) {
      errors.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const auto inbound_size = inbound.value()->Size();
    if (!inbound_size.ok() || inbound_size.value() != 0)
      errors.fetch_add(1, std::memory_order_relaxed);
  }

  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  std::cout << "host=" << config.local_host_id
            << " peers=3 queues=12 iterations=" << iterations
            << " operations=" << iterations * 6 << " elapsed_ms=" << elapsed_ms
            << " poller_cpu=" << poller->bound_cpu()
            << " batch_size=" << poller_options.batch_size
            << " batches=" << stats.batches
            << " empty_scans=" << stats.empty_scans
            << " yields=" << stats.yields << " sleeps=" << stats.sleeps
            << " errors=" << errors.load(std::memory_order_relaxed) << "\n";
  runtime.Finalize();
  return errors.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
