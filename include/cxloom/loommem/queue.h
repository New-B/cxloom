#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cxloom/common/messages.h"
#include "cxloom/common/status.h"
#include "cxloom/loommem/visibility.h"

namespace cxloom::loommem {

inline constexpr std::uint64_t kQueueRegionMagic = 0x43584c4f4f4d5152ULL;
inline constexpr std::uint64_t kSpscQueueMagic = 0x43584c4f4f4d5351ULL;
inline constexpr std::uint32_t kQueueLayoutVersion = 1;
inline constexpr std::size_t kQueuePayloadBytes = 128;

struct QueueEnvelope {
  MessageHeader header{};
  std::vector<std::byte> payload;
};

struct alignas(64) QueueRegionHeader {
  std::uint64_t magic{0};
  std::uint32_t layout_version{0};
  std::uint32_t header_bytes{0};
  std::uint16_t host_count{0};
  std::uint16_t reserved0{0};
  std::uint32_t capacity_entries{0};
  std::uint32_t slot_bytes{0};
  std::uint32_t queue_header_bytes{0};
  std::uint64_t queue_stride{0};
  std::uint64_t queue_count{0};
  std::uint64_t required_bytes{0};
};

struct alignas(64) SpscQueueIdentity {
  std::uint64_t magic{0};
  HostId producer{0};
  HostId consumer{0};
  std::uint32_t capacity_entries{0};
  std::uint32_t slot_bytes{0};
};

struct alignas(64) SpscProducerCursor {
  std::atomic<std::uint64_t> tail{0};
};

struct alignas(64) SpscConsumerCursor {
  std::atomic<std::uint64_t> head{0};
};

struct alignas(64) SharedSpscQueueHeader {
  SpscQueueIdentity identity{};
  SpscProducerCursor producer{};
  SpscConsumerCursor consumer{};
};

struct alignas(64) SharedSpscQueueSlot {
  std::atomic<std::uint64_t> sequence{0};
  MessageHeader header{};
  std::array<std::byte, kQueuePayloadBytes> payload{};
};

struct SharedQueueStorage {
  SharedSpscQueueHeader *header{nullptr};
  SharedSpscQueueSlot *slots{nullptr};
};

Status FormatSharedQueueRegion(void *region, std::size_t region_bytes,
                               std::uint16_t host_count,
                               std::size_t capacity_entries);
Status ValidateSharedQueueRegion(const void *region, std::size_t region_bytes,
                                 std::uint16_t host_count,
                                 std::size_t capacity_entries);
Result<SharedQueueStorage> LocateSharedQueue(void *region,
                                             std::size_t region_bytes,
                                             HostId producer, HostId consumer);

class SpscQueue {
public:
  SpscQueue(SharedQueueStorage storage, HostId local_host,
            VisibilityMode visibility_mode = VisibilityMode::kReleaseAcquire);

  Status Push(const QueueEnvelope &message);
  Status TryPop(QueueEnvelope *message, bool *popped);
  Result<QueueEnvelope> Pop();
  Result<std::size_t> Size() const;
  std::size_t capacity() const;
  HostId producer() const;
  HostId consumer() const;

private:
  SharedSpscQueueHeader *header_{nullptr};
  SharedSpscQueueSlot *slots_{nullptr};
  HostId local_host_{0};
  VisibilityMode visibility_mode_{VisibilityMode::kReleaseAcquire};
};

} // namespace cxloom::loommem
