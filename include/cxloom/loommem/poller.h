#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "cxloom/common/status.h"
#include "cxloom/loommem/queue.h"

namespace cxloom::loommem {

struct QueuePollerOptions {
  int cpu_id{-1};
  std::size_t batch_size{32};
  std::uint32_t spin_rounds{4096};
  std::uint32_t yield_rounds{64};
  std::uint32_t idle_sleep_us{50};
};

struct QueuePollerStats {
  std::uint64_t scans{0};
  std::uint64_t empty_scans{0};
  std::uint64_t messages{0};
  std::uint64_t batches{0};
  std::uint64_t yields{0};
  std::uint64_t sleeps{0};
};

using QueueMessageHandler = std::function<Status(QueueEnvelope)>;

class QueuePoller {
public:
  QueuePoller(HostId local_host, std::vector<SpscQueue *> inbound_queues,
              QueueMessageHandler handler, QueuePollerOptions options = {});
  ~QueuePoller();

  QueuePoller(const QueuePoller &) = delete;
  QueuePoller &operator=(const QueuePoller &) = delete;

  Status Start();
  Status Stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  int bound_cpu() const { return bound_cpu_; }
  QueuePollerStats stats() const;
  Status terminal_status() const;

private:
  void Run();
  bool ScanOnce();
  void RecordTerminalStatus(Status status);
  Result<int> SelectCpu() const;

  HostId local_host_{0};
  std::vector<SpscQueue *> inbound_queues_;
  QueueMessageHandler handler_;
  QueuePollerOptions options_{};
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> start_gate_{false};
  std::size_t next_queue_{0};
  int bound_cpu_{-1};

  std::atomic<std::uint64_t> scans_{0};
  std::atomic<std::uint64_t> empty_scans_{0};
  std::atomic<std::uint64_t> messages_{0};
  std::atomic<std::uint64_t> batches_{0};
  std::atomic<std::uint64_t> yields_{0};
  std::atomic<std::uint64_t> sleeps_{0};

  mutable std::mutex status_mutex_;
  Status terminal_status_{};
};

} // namespace cxloom::loommem
