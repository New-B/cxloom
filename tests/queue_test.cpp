#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "cxloom/loommem/queue.h"

namespace {

cxloom::loommem::QueueEnvelope Message(cxloom::HostId source,
                                       cxloom::HostId destination,
                                       std::uint64_t value) {
  cxloom::loommem::QueueEnvelope message;
  message.header.kind = cxloom::MessageKind::kTokenReq;
  message.header.src_host = source;
  message.header.dst_host = destination;
  message.header.payload_bytes = sizeof(value);
  message.payload.resize(sizeof(value));
  std::memcpy(message.payload.data(), &value, sizeof(value));
  return message;
}

std::uint64_t Value(const cxloom::loommem::QueueEnvelope &message) {
  std::uint64_t value = 0;
  std::memcpy(&value, message.payload.data(), sizeof(value));
  return value;
}

} // namespace

int main() {
  alignas(64) std::byte region[1 << 20]{};
  constexpr std::size_t kCapacity = 4;
  auto status = cxloom::loommem::FormatSharedQueueRegion(region, sizeof(region),
                                                         3, kCapacity);
  if (!status.ok() ||
      !cxloom::loommem::ValidateSharedQueueRegion(region, sizeof(region), 3,
                                                  kCapacity)
           .ok() ||
      cxloom::loommem::ValidateSharedQueueRegion(region, sizeof(region), 3,
                                                 kCapacity + 1)
          .ok() ||
      cxloom::loommem::LocateSharedQueue(region, sizeof(region), 1, 1).ok() ||
      cxloom::loommem::FormatSharedQueueRegion(region, sizeof(region), 3,
                                               100000)
          .ok()) {
    std::cerr << "shared queue region formatting or validation failed\n";
    return 1;
  }

  const auto storage =
      cxloom::loommem::LocateSharedQueue(region, sizeof(region), 0, 1);
  if (!storage.ok())
    return 1;
  cxloom::loommem::SpscQueue producer(storage.value(), 0);
  cxloom::loommem::SpscQueue consumer(storage.value(), 1);
  cxloom::loommem::SpscQueue wrong_endpoint(storage.value(), 2);
  auto oversized = Message(0, 1, 0);
  oversized.payload.resize(cxloom::loommem::kQueuePayloadBytes + 1);
  oversized.header.payload_bytes = oversized.payload.size();
  if (producer.Push(oversized).ok() || producer.Push(Message(0, 2, 0)).ok()) {
    std::cerr << "queue accepted an invalid endpoint or oversized payload\n";
    return 1;
  }

  for (std::uint64_t value = 0; value < kCapacity; ++value) {
    if (!producer.Push(Message(0, 1, value)).ok())
      return 1;
  }
  const auto full_size = producer.Size();
  if (!full_size.ok() || full_size.value() != kCapacity ||
      producer.Push(Message(0, 1, kCapacity)).ok() ||
      wrong_endpoint.Push(Message(0, 1, 0)).ok() || wrong_endpoint.Pop().ok()) {
    std::cerr << "queue capacity, backpressure, or endpoint ownership failed\n";
    return 1;
  }
  for (std::uint64_t value = 0; value < kCapacity; ++value) {
    const auto message = consumer.Pop();
    if (!message.ok() || Value(message.value()) != value)
      return 1;
  }
  if (consumer.Pop().ok())
    return 1;

  constexpr std::uint64_t kIterations = 100000;
  std::atomic<bool> failed{false};
  std::thread writer([&] {
    for (std::uint64_t value = 0; value < kIterations;) {
      const auto push = producer.Push(Message(0, 1, value));
      if (push.ok()) {
        ++value;
      } else if (push.code() != cxloom::StatusCode::kUnavailable) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  });
  std::thread reader([&] {
    for (std::uint64_t expected = 0; expected < kIterations;) {
      const auto message = consumer.Pop();
      if (message.ok()) {
        if (Value(message.value()) != expected) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        ++expected;
      } else if (message.status().code() != cxloom::StatusCode::kUnavailable) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  });
  writer.join();
  reader.join();

  const auto final_size = consumer.Size();
  if (failed.load(std::memory_order_relaxed) || !final_size.ok() ||
      final_size.value() != 0) {
    std::cerr << "concurrent SPSC ordering or wrap-around failed\n";
    return 1;
  }
  return 0;
}
