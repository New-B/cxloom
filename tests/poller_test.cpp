#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "cxloom/loommem/poller.h"

namespace {

cxloom::loommem::QueueEnvelope Message(cxloom::HostId source,
                                       std::uint64_t sequence) {
  cxloom::loommem::QueueEnvelope message;
  message.header.kind = cxloom::MessageKind::kTokenReq;
  message.header.src_host = source;
  message.header.dst_host = 1;
  message.header.payload_bytes = sizeof(sequence);
  message.payload.resize(sizeof(sequence));
  std::memcpy(message.payload.data(), &sequence, sizeof(sequence));
  return message;
}

std::uint64_t Sequence(const cxloom::loommem::QueueEnvelope &message) {
  std::uint64_t sequence = 0;
  std::memcpy(&sequence, message.payload.data(), sizeof(sequence));
  return sequence;
}

} // namespace

int main() {
  alignas(64) std::byte region[1 << 20]{};
  constexpr std::size_t kCapacity = 64;
  constexpr std::uint64_t kMessagesPerProducer = 10000;
  if (!cxloom::loommem::FormatSharedQueueRegion(region, sizeof(region), 3,
                                                kCapacity)
           .ok())
    return 1;

  const auto storage0 =
      cxloom::loommem::LocateSharedQueue(region, sizeof(region), 0, 1);
  const auto storage2 =
      cxloom::loommem::LocateSharedQueue(region, sizeof(region), 2, 1);
  if (!storage0.ok() || !storage2.ok())
    return 1;
  cxloom::loommem::SpscQueue producer0(storage0.value(), 0);
  cxloom::loommem::SpscQueue inbound0(storage0.value(), 1);
  cxloom::loommem::SpscQueue producer2(storage2.value(), 2);
  cxloom::loommem::SpscQueue inbound2(storage2.value(), 1);

  std::uint64_t expected[3]{0, 0, 0};
  std::atomic<std::uint64_t> received{0};
  cxloom::loommem::QueuePollerOptions options;
  options.batch_size = 8;
  options.spin_rounds = 8;
  options.yield_rounds = 4;
  options.idle_sleep_us = 50;
  cxloom::loommem::QueuePoller poller(
      1, {&inbound0, &inbound2},
      [&](cxloom::loommem::QueueEnvelope message) {
        const auto source = message.header.src_host;
        if ((source != 0 && source != 2) ||
            Sequence(message) != expected[source])
          return cxloom::Status::Internal(
              "poller violated per-source FIFO ordering");
        ++expected[source];
        received.fetch_add(1, std::memory_order_release);
        return cxloom::Status::Ok();
      },
      options);
  if (!poller.Start().ok() || poller.bound_cpu() < 0)
    return 1;

  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  std::atomic<bool> failed{false};
  auto send = [&](cxloom::loommem::SpscQueue &queue, cxloom::HostId source) {
    for (std::uint64_t sequence = 0; sequence < kMessagesPerProducer;) {
      const auto status = queue.Push(Message(source, sequence));
      if (status.ok()) {
        ++sequence;
      } else if (status.code() != cxloom::StatusCode::kUnavailable) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  };
  std::thread writer0(send, std::ref(producer0), 0);
  std::thread writer2(send, std::ref(producer2), 2);
  writer0.join();
  writer2.join();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received.load(std::memory_order_acquire) != 2 * kMessagesPerProducer &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto stop_status = poller.Stop();
  const auto stats = poller.stats();
  if (failed.load(std::memory_order_relaxed) || !stop_status.ok() ||
      received.load(std::memory_order_relaxed) != 2 * kMessagesPerProducer ||
      expected[0] != kMessagesPerProducer ||
      expected[2] != kMessagesPerProducer ||
      stats.messages != 2 * kMessagesPerProducer || stats.batches == 0 ||
      stats.scans == 0 || stats.empty_scans == 0 || stats.sleeps == 0) {
    std::cerr << "bound round-robin batch poller test failed\n";
    return 1;
  }
  return 0;
}
