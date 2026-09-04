#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

namespace {

struct Record {
  std::uint64_t sequence{0};
  std::uint64_t writer{0};
  std::uint64_t checksum{0};
  std::uint64_t payload[29]{};
};

std::uint64_t Parse(const char *value, std::uint64_t fallback) {
  if (value == nullptr || *value == '\0')
    return fallback;
  char *end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  return end != value && *end == '\0' ? parsed : fallback;
}

Record MakeRecord(std::uint64_t sequence, cxloom::HostId writer) {
  Record record;
  record.sequence = sequence;
  record.writer = writer;
  record.checksum = sequence ^ writer;
  for (std::size_t index = 0; index < 29; ++index) {
    record.payload[index] = sequence * 131 + writer * 17 + index;
    record.checksum ^= record.payload[index];
  }
  return record;
}

bool Valid(const cxloom::loommem::ReadSnapshot &snapshot,
           std::uint16_t host_count) {
  if (snapshot.data() == nullptr || snapshot.bytes() != sizeof(Record))
    return false;
  const auto &record = *static_cast<const Record *>(snapshot.data());
  if (snapshot.version == 0)
    return record.sequence == 0;
  const auto writer =
      static_cast<cxloom::HostId>((snapshot.version - 1) % host_count);
  const auto expected = MakeRecord(snapshot.version, writer);
  if (record.sequence != expected.sequence || record.writer != expected.writer ||
      record.checksum != expected.checksum)
    return false;
  for (std::size_t index = 0; index < 29; ++index) {
    if (record.payload[index] != expected.payload[index])
      return false;
  }
  return true;
}

} // namespace

int main() {
  const char *path = std::getenv("CL_DAX_DEVICE");
  if (path == nullptr || *path == '\0')
    return 2;

  cxloom::CxloomConfig config;
  config.local_host_id =
      static_cast<cxloom::HostId>(Parse(std::getenv("CL_HOST_ID"), 0));
  config.host_count =
      static_cast<std::uint16_t>(Parse(std::getenv("CL_HOST_COUNT"), 0));
  config.shared_region_bytes =
      Parse(std::getenv("CL_SHARED_REGION_BYTES"), config.shared_region_bytes);
  config.shared_region_path = path;
  config.bootstrap_owner = Parse(std::getenv("CL_BOOTSTRAP_OWNER"), 0) != 0;
  config.bootstrap_timeout_ms =
      Parse(std::getenv("CL_COHERENCE_TIMEOUT_MS"), 120000);
  const auto iterations =
      Parse(std::getenv("CL_COHERENCE_ITERATIONS"), 1000);
  if (config.host_count < 2 || config.host_count > cxloom::kMaxHosts ||
      config.local_host_id >= config.host_count || iterations == 0)
    return 2;

  cxloom::loommem::LoomMemRuntime runtime(config);
  auto status = runtime.Initialize();
  if (!status.ok()) {
    std::cerr << "initialization failed: " << status.message() << "\n";
    return 1;
  }

  const auto allocation =
      runtime.AllocateShared(sizeof(Record), alignof(Record));
  if (!allocation.ok())
    return 1;
  const auto local = runtime.ResolveLocal(allocation.value());
  if (!local.ok())
    return 1;
  *static_cast<Record *>(local.value()) = Record{};
  status = runtime.PublishSharedObject(allocation.value(), sizeof(Record));
  if (status.ok())
    status = runtime.WaitForAllSharedObjects(config.bootstrap_timeout_ms);
  if (!status.ok()) {
    std::cerr << "object rendezvous failed: " << status.message() << "\n";
    return 1;
  }
  const auto published = runtime.ReadPublishedSharedObject(0);
  if (!published.ok())
    return 1;
  const auto object = published.value().gptr;

  status = runtime.StartQueuePoller();
  if (!status.ok())
    return 1;

  std::uint64_t errors = 0;
  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t round = 0; round < iterations; ++round) {
    const auto writer = static_cast<cxloom::HostId>(round % config.host_count);
    if (writer == config.local_host_id) {
      auto write = runtime.AcquireWriteBuffer(object, config.bootstrap_timeout_ms);
      if (!write.ok() || write.value().bytes() != sizeof(Record)) {
        ++errors;
      } else {
        *static_cast<Record *>(write.value().data()) =
            MakeRecord(round + 1, writer);
        if (!runtime.ReleaseWriteBuffer(write.value()).ok())
          ++errors;
      }
    }

    const auto snapshot =
        runtime.AcquireReadSnapshot(object, config.bootstrap_timeout_ms);
    if (!snapshot.ok() || snapshot.value().version > round + 1 ||
        !Valid(snapshot.value(), config.host_count))
      ++errors;

    status = runtime.PublishVisibilitySequence(
        round + 1, cxloom::loommem::VisibilityMode::kReleaseAcquire);
    if (status.ok())
      status = runtime.WaitForVisibilitySequence(
          round + 1, config.bootstrap_timeout_ms,
          cxloom::loommem::VisibilityMode::kReleaseAcquire);
    if (!status.ok()) {
      ++errors;
      break;
    }
  }

  const auto final_snapshot =
      runtime.AcquireReadSnapshot(object, config.bootstrap_timeout_ms);
  if (!final_snapshot.ok() || final_snapshot.value().version != iterations ||
      !Valid(final_snapshot.value(), config.host_count))
    ++errors;

  const auto stop = runtime.StopQueuePoller();
  if (!stop.ok())
    ++errors;
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  std::cout << "host=" << config.local_host_id
            << " hosts=" << config.host_count << " iterations=" << iterations
            << " final_version="
            << (final_snapshot.ok() ? final_snapshot.value().version : 0)
            << " elapsed_ms=" << elapsed_ms << " errors=" << errors << "\n";
  runtime.Finalize();
  return errors == 0 ? 0 : 1;
}
