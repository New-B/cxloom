#include "cxloom/loommem/queue.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace cxloom::loommem {
namespace {

std::uint64_t QueueCount(std::uint16_t host_count) {
  return static_cast<std::uint64_t>(host_count) *
         (host_count == 0 ? 0 : host_count - 1);
}

Result<std::uint64_t> RequiredBytes(std::uint16_t host_count,
                                    std::size_t capacity_entries) {
  if (host_count == 0 || host_count > kMaxHosts || capacity_entries == 0 ||
      capacity_entries > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("invalid shared queue dimensions");
  }
  if (capacity_entries > (std::numeric_limits<std::uint64_t>::max() -
                          sizeof(SharedSpscQueueHeader)) /
                             sizeof(SharedSpscQueueSlot)) {
    return Status::InvalidArgument("shared queue stride overflows");
  }
  const auto stride = sizeof(SharedSpscQueueHeader) +
                      capacity_entries * sizeof(SharedSpscQueueSlot);
  const auto count = QueueCount(host_count);
  if (count != 0 && stride > (std::numeric_limits<std::uint64_t>::max() -
                              sizeof(QueueRegionHeader)) /
                                 count) {
    return Status::InvalidArgument("shared queue region size overflows");
  }
  return sizeof(QueueRegionHeader) + count * stride;
}

Result<std::uint64_t> PairIndex(std::uint16_t host_count, HostId producer,
                                HostId consumer) {
  if (producer >= host_count || consumer >= host_count) {
    return Status::InvalidArgument("queue endpoint is out of range");
  }
  if (producer == consumer) {
    return Status::InvalidArgument(
        "self-pairs do not use the shared queue transport");
  }
  const auto consumer_index = consumer < producer ? consumer : consumer - 1;
  return static_cast<std::uint64_t>(producer) * (host_count - 1) +
         consumer_index;
}

Status ValidateHeader(const QueueRegionHeader &header, std::size_t region_bytes,
                      std::uint16_t host_count, std::size_t capacity_entries) {
  const auto required = RequiredBytes(host_count, capacity_entries);
  if (!required.ok())
    return required.status();
  if (header.magic != kQueueRegionMagic ||
      header.layout_version != kQueueLayoutVersion ||
      header.header_bytes != sizeof(QueueRegionHeader) ||
      header.host_count != host_count ||
      header.capacity_entries != capacity_entries ||
      header.slot_bytes != sizeof(SharedSpscQueueSlot) ||
      header.queue_header_bytes != sizeof(SharedSpscQueueHeader) ||
      header.queue_stride !=
          sizeof(SharedSpscQueueHeader) +
              capacity_entries * sizeof(SharedSpscQueueSlot) ||
      header.queue_count != QueueCount(host_count) ||
      header.required_bytes != required.value()) {
    return Status::FailedPrecondition(
        "shared queue region has an incompatible layout");
  }
  if (header.required_bytes > region_bytes) {
    return Status::FailedPrecondition(
        "shared queue region exceeds its reserved layout range");
  }
  return Status::Ok();
}

} // namespace

static_assert(sizeof(QueueRegionHeader) == 64);
static_assert(sizeof(SpscQueueIdentity) == 64);
static_assert(sizeof(SpscProducerCursor) == 64);
static_assert(sizeof(SpscConsumerCursor) == 64);
static_assert(sizeof(SharedSpscQueueHeader) == 192);
static_assert(sizeof(SharedSpscQueueSlot) == 192);
static_assert(std::is_trivially_copyable_v<MessageHeader>);
static_assert(offsetof(SharedSpscQueueSlot, payload) ==
              offsetof(SharedSpscQueueSlot, header) + sizeof(MessageHeader));

Status FormatSharedQueueRegion(void *region, std::size_t region_bytes,
                               std::uint16_t host_count,
                               std::size_t capacity_entries) {
  if (region == nullptr)
    return Status::InvalidArgument("shared queue region is null");
  const auto required = RequiredBytes(host_count, capacity_entries);
  if (!required.ok())
    return required.status();
  if (required.value() > region_bytes) {
    return Status::InvalidArgument(
        "reserved queue region is too small for host count and queue capacity");
  }

  std::memset(region, 0, required.value());
  auto *region_header = new (region) QueueRegionHeader();
  region_header->magic = kQueueRegionMagic;
  region_header->layout_version = kQueueLayoutVersion;
  region_header->header_bytes = sizeof(QueueRegionHeader);
  region_header->host_count = host_count;
  region_header->capacity_entries =
      static_cast<std::uint32_t>(capacity_entries);
  region_header->slot_bytes = sizeof(SharedSpscQueueSlot);
  region_header->queue_header_bytes = sizeof(SharedSpscQueueHeader);
  region_header->queue_stride = sizeof(SharedSpscQueueHeader) +
                                capacity_entries * sizeof(SharedSpscQueueSlot);
  region_header->queue_count = QueueCount(host_count);
  region_header->required_bytes = required.value();

  auto *base = static_cast<std::byte *>(region);
  for (HostId producer = 0; producer < host_count; ++producer) {
    for (HostId consumer = 0; consumer < host_count; ++consumer) {
      if (producer == consumer)
        continue;
      const auto index = PairIndex(host_count, producer, consumer);
      if (!index.ok())
        return index.status();
      auto *queue = new (base + sizeof(QueueRegionHeader) +
                         index.value() * region_header->queue_stride)
          SharedSpscQueueHeader();
      queue->identity.magic = kSpscQueueMagic;
      queue->identity.producer = producer;
      queue->identity.consumer = consumer;
      queue->identity.capacity_entries =
          static_cast<std::uint32_t>(capacity_entries);
      queue->identity.slot_bytes = sizeof(SharedSpscQueueSlot);
      auto *slots = reinterpret_cast<SharedSpscQueueSlot *>(
          reinterpret_cast<std::byte *>(queue) + sizeof(SharedSpscQueueHeader));
      for (std::size_t slot = 0; slot < capacity_entries; ++slot)
        new (&slots[slot]) SharedSpscQueueSlot();
    }
  }
  return Status::Ok();
}

Status ValidateSharedQueueRegion(const void *region, std::size_t region_bytes,
                                 std::uint16_t host_count,
                                 std::size_t capacity_entries) {
  if (region == nullptr || region_bytes < sizeof(QueueRegionHeader)) {
    return Status::InvalidArgument("shared queue region is too small");
  }
  const auto &region_header = *static_cast<const QueueRegionHeader *>(region);
  const auto header_status =
      ValidateHeader(region_header, region_bytes, host_count, capacity_entries);
  if (!header_status.ok())
    return header_status;

  auto *base = static_cast<const std::byte *>(region);
  for (HostId producer = 0; producer < host_count; ++producer) {
    for (HostId consumer = 0; consumer < host_count; ++consumer) {
      if (producer == consumer)
        continue;
      const auto index = PairIndex(host_count, producer, consumer);
      if (!index.ok())
        return index.status();
      const auto *queue = reinterpret_cast<const SharedSpscQueueHeader *>(
          base + sizeof(QueueRegionHeader) +
          index.value() * region_header.queue_stride);
      const auto *slots = reinterpret_cast<const SharedSpscQueueSlot *>(
          reinterpret_cast<const std::byte *>(queue) +
          sizeof(SharedSpscQueueHeader));
      if (queue->identity.magic != kSpscQueueMagic ||
          queue->identity.producer != producer ||
          queue->identity.consumer != consumer ||
          queue->identity.capacity_entries != capacity_entries ||
          queue->identity.slot_bytes != sizeof(SharedSpscQueueSlot) ||
          !queue->producer.tail.is_lock_free() ||
          !queue->consumer.head.is_lock_free() ||
          !slots[0].sequence.is_lock_free()) {
        return Status::FailedPrecondition(
            "shared SPSC queue metadata is incompatible");
      }
    }
  }
  return Status::Ok();
}

Result<SharedQueueStorage> LocateSharedQueue(void *region,
                                             std::size_t region_bytes,
                                             HostId producer, HostId consumer) {
  if (region == nullptr || region_bytes < sizeof(QueueRegionHeader)) {
    return Status::InvalidArgument("shared queue region is too small");
  }
  auto *region_header = static_cast<QueueRegionHeader *>(region);
  const auto header_status =
      ValidateHeader(*region_header, region_bytes, region_header->host_count,
                     region_header->capacity_entries);
  if (!header_status.ok())
    return header_status;
  const auto index = PairIndex(region_header->host_count, producer, consumer);
  if (!index.ok())
    return index.status();
  auto *queue = reinterpret_cast<SharedSpscQueueHeader *>(
      static_cast<std::byte *>(region) + sizeof(QueueRegionHeader) +
      index.value() * region_header->queue_stride);
  if (queue->identity.magic != kSpscQueueMagic ||
      queue->identity.producer != producer ||
      queue->identity.consumer != consumer) {
    return Status::FailedPrecondition(
        "shared SPSC queue identity is incompatible");
  }
  auto *slots = reinterpret_cast<SharedSpscQueueSlot *>(
      reinterpret_cast<std::byte *>(queue) + sizeof(SharedSpscQueueHeader));
  return SharedQueueStorage{queue, slots};
}

SpscQueue::SpscQueue(SharedQueueStorage storage, HostId local_host,
                     VisibilityMode visibility_mode)
    : header_(storage.header), slots_(storage.slots), local_host_(local_host),
      visibility_mode_(visibility_mode) {}

Status SpscQueue::Push(const QueueEnvelope &message) {
  if (header_ == nullptr || slots_ == nullptr)
    return Status::FailedPrecondition("shared SPSC queue is not attached");
  if (local_host_ != producer())
    return Status::FailedPrecondition(
        "only the configured producer may push to this queue");
  if (message.header.kind == MessageKind::kInvalid ||
      message.header.src_host != producer() ||
      message.header.dst_host != consumer() ||
      message.header.payload_bytes != message.payload.size()) {
    return Status::InvalidArgument(
        "message header does not match queue endpoints or payload");
  }
  if (message.payload.size() > kQueuePayloadBytes)
    return Status::InvalidArgument(
        "message payload exceeds the shared queue slot capacity");

  auto &producer_cursor = header_->producer.tail;
  auto &consumer_cursor = header_->consumer.head;
  auto acquire_status =
      AcquireData(&consumer_cursor, sizeof(consumer_cursor), visibility_mode_);
  if (!acquire_status.ok())
    return acquire_status;
  const auto tail = producer_cursor.load(std::memory_order_relaxed);
  const auto head = consumer_cursor.load(std::memory_order_acquire);
  if (tail < head)
    return Status::Internal("shared SPSC queue cursors are corrupt");
  if (tail - head >= capacity())
    return Status::Unavailable("queue is full");

  auto &slot = slots_[tail % capacity()];
  slot.header = message.header;
  if (!message.payload.empty())
    std::memcpy(slot.payload.data(), message.payload.data(),
                message.payload.size());
  const auto payload_status =
      PublishData(&slot.header, sizeof(slot.header) + message.payload.size(),
                  visibility_mode_);
  if (!payload_status.ok())
    return payload_status;
  slot.sequence.store(tail + 1, std::memory_order_release);
  const auto sequence_status =
      PublishData(&slot.sequence, sizeof(slot.sequence), visibility_mode_);
  if (!sequence_status.ok())
    return sequence_status;
  producer_cursor.store(tail + 1, std::memory_order_release);
  return PublishData(&producer_cursor, sizeof(producer_cursor),
                     visibility_mode_);
}

Status SpscQueue::TryPop(QueueEnvelope *message, bool *popped) {
  if (message == nullptr || popped == nullptr)
    return Status::InvalidArgument("TryPop requires output pointers");
  *popped = false;
  if (header_ == nullptr || slots_ == nullptr)
    return Status::FailedPrecondition("shared SPSC queue is not attached");
  if (local_host_ != consumer())
    return Status::FailedPrecondition(
        "only the configured consumer may pop from this queue");

  auto &producer_cursor = header_->producer.tail;
  auto &consumer_cursor = header_->consumer.head;
  auto acquire_status =
      AcquireData(&producer_cursor, sizeof(producer_cursor), visibility_mode_);
  if (!acquire_status.ok())
    return acquire_status;
  const auto head = consumer_cursor.load(std::memory_order_relaxed);
  const auto tail = producer_cursor.load(std::memory_order_acquire);
  if (head == tail)
    return Status::Ok();
  if (tail < head || tail - head > capacity())
    return Status::Internal("shared SPSC queue cursors are corrupt");

  auto &slot = slots_[head % capacity()];
  acquire_status = AcquireData(&slot, sizeof(slot), visibility_mode_);
  if (!acquire_status.ok())
    return acquire_status;
  if (slot.sequence.load(std::memory_order_acquire) != head + 1)
    return Status::Ok();
  if (slot.header.kind == MessageKind::kInvalid ||
      slot.header.src_host != producer() ||
      slot.header.dst_host != consumer() ||
      slot.header.payload_bytes > kQueuePayloadBytes) {
    return Status::Internal("shared SPSC queue message is corrupt");
  }

  message->header = slot.header;
  message->payload.resize(message->header.payload_bytes);
  if (!message->payload.empty())
    std::memcpy(message->payload.data(), slot.payload.data(),
                message->payload.size());

  consumer_cursor.store(head + 1, std::memory_order_release);
  const auto publish_status =
      PublishData(&consumer_cursor, sizeof(consumer_cursor), visibility_mode_);
  if (!publish_status.ok())
    return publish_status;
  *popped = true;
  return Status::Ok();
}

Result<QueueEnvelope> SpscQueue::Pop() {
  QueueEnvelope message;
  bool popped = false;
  const auto status = TryPop(&message, &popped);
  if (!status.ok())
    return status;
  if (!popped)
    return Status::Unavailable("queue is empty");
  return message;
}

Result<std::size_t> SpscQueue::Size() const {
  if (header_ == nullptr)
    return Status::FailedPrecondition("shared SPSC queue is not attached");
  const auto acquire_tail =
      AcquireData(&header_->producer.tail, sizeof(header_->producer.tail),
                  visibility_mode_);
  if (!acquire_tail.ok())
    return acquire_tail;
  const auto acquire_head =
      AcquireData(&header_->consumer.head, sizeof(header_->consumer.head),
                  visibility_mode_);
  if (!acquire_head.ok())
    return acquire_head;
  const auto tail = header_->producer.tail.load(std::memory_order_acquire);
  const auto head = header_->consumer.head.load(std::memory_order_acquire);
  if (tail < head || tail - head > capacity())
    return Status::Internal("shared SPSC queue cursors are corrupt");
  return static_cast<std::size_t>(tail - head);
}

std::size_t SpscQueue::capacity() const {
  return header_ == nullptr ? 0 : header_->identity.capacity_entries;
}

HostId SpscQueue::producer() const {
  return header_ == nullptr ? 0 : header_->identity.producer;
}

HostId SpscQueue::consumer() const {
  return header_ == nullptr ? 0 : header_->identity.consumer;
}

} // namespace cxloom::loommem
