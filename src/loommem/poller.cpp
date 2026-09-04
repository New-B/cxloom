#include "cxloom/loommem/poller.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <unordered_set>

#include <pthread.h>
#include <sched.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace cxloom::loommem {
namespace {

void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

} // namespace

QueuePoller::QueuePoller(HostId local_host,
                         std::vector<SpscQueue *> inbound_queues,
                         QueueMessageHandler handler,
                         QueuePollerOptions options)
    : local_host_(local_host), inbound_queues_(std::move(inbound_queues)),
      handler_(std::move(handler)), options_(options) {}

QueuePoller::~QueuePoller() { Stop(); }

Result<int> QueuePoller::SelectCpu() const {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    return Status::Internal("failed to read process CPU affinity");

  if (options_.cpu_id >= 0) {
    if (options_.cpu_id >= CPU_SETSIZE || !CPU_ISSET(options_.cpu_id, &allowed))
      return Status::InvalidArgument(
          "poller CPU is outside the process affinity mask");
    return options_.cpu_id;
  }
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &allowed))
      return cpu;
  }
  return Status::FailedPrecondition("process affinity mask contains no CPU");
}

Status QueuePoller::Start() {
  if (running_.load(std::memory_order_acquire) || worker_.joinable())
    return Status::AlreadyExists("queue poller is already running");
  if (!handler_)
    return Status::InvalidArgument("queue poller requires a message handler");
  if (inbound_queues_.empty())
    return Status::InvalidArgument(
        "queue poller requires at least one inbound queue");
  if (options_.batch_size == 0)
    return Status::InvalidArgument("queue poller batch size must be non-zero");

  std::unordered_set<const SpscQueue *> unique_queues;
  for (const auto *queue : inbound_queues_) {
    if (queue == nullptr || queue->consumer() != local_host_ ||
        !unique_queues.insert(queue).second)
      return Status::InvalidArgument(
          "queue poller received an invalid or duplicate inbound queue");
  }

  const auto cpu = SelectCpu();
  if (!cpu.ok())
    return cpu.status();
  bound_cpu_ = cpu.value();
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    terminal_status_ = Status::Ok();
  }
  next_queue_ = 0;
  scans_.store(0, std::memory_order_relaxed);
  empty_scans_.store(0, std::memory_order_relaxed);
  messages_.store(0, std::memory_order_relaxed);
  batches_.store(0, std::memory_order_relaxed);
  yields_.store(0, std::memory_order_relaxed);
  sleeps_.store(0, std::memory_order_relaxed);
  start_gate_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_release);
  worker_ = std::thread(&QueuePoller::Run, this);

  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  CPU_SET(bound_cpu_, &affinity);
  const int affinity_error = pthread_setaffinity_np(
      worker_.native_handle(), sizeof(affinity), &affinity);
  if (affinity_error != 0) {
    running_.store(false, std::memory_order_release);
    start_gate_.store(true, std::memory_order_release);
    worker_.join();
    return Status::Internal("failed to bind queue poller to CPU " +
                            std::to_string(bound_cpu_) + ": " +
                            std::strerror(affinity_error));
  }
  start_gate_.store(true, std::memory_order_release);
  return Status::Ok();
}

Status QueuePoller::Stop() {
  running_.store(false, std::memory_order_release);
  start_gate_.store(true, std::memory_order_release);
  if (worker_.joinable()) {
    if (worker_.get_id() == std::this_thread::get_id())
      return Status::FailedPrecondition(
          "queue poller cannot join itself from its handler");
    worker_.join();
  }
  return terminal_status();
}

void QueuePoller::RecordTerminalStatus(Status status) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  if (terminal_status_.ok())
    terminal_status_ = std::move(status);
}

Status QueuePoller::terminal_status() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return terminal_status_;
}

QueuePollerStats QueuePoller::stats() const {
  return {
      scans_.load(std::memory_order_relaxed),
      empty_scans_.load(std::memory_order_relaxed),
      messages_.load(std::memory_order_relaxed),
      batches_.load(std::memory_order_relaxed),
      yields_.load(std::memory_order_relaxed),
      sleeps_.load(std::memory_order_relaxed),
  };
}

bool QueuePoller::ScanOnce() {
  const auto queue_count = inbound_queues_.size();
  const auto start = next_queue_;
  bool progressed = false;
  for (std::size_t offset = 0; offset < queue_count; ++offset) {
    auto *queue = inbound_queues_[(start + offset) % queue_count];
    std::size_t drained = 0;
    while (drained < options_.batch_size) {
      QueueEnvelope message;
      bool popped = false;
      const auto pop_status = queue->TryPop(&message, &popped);
      if (!pop_status.ok()) {
        RecordTerminalStatus(pop_status);
        running_.store(false, std::memory_order_release);
        break;
      }
      if (!popped)
        break;
      try {
        const auto status = handler_(std::move(message));
        if (!status.ok()) {
          RecordTerminalStatus(status);
          running_.store(false, std::memory_order_release);
          return true;
        }
      } catch (const std::exception &error) {
        RecordTerminalStatus(Status::Internal(
            std::string("queue handler threw: ") + error.what()));
        running_.store(false, std::memory_order_release);
        return true;
      } catch (...) {
        RecordTerminalStatus(
            Status::Internal("queue handler threw an unknown exception"));
        running_.store(false, std::memory_order_release);
        return true;
      }
      ++drained;
      messages_.fetch_add(1, std::memory_order_relaxed);
      progressed = true;
    }
    if (drained != 0)
      batches_.fetch_add(1, std::memory_order_relaxed);
    if (!running_.load(std::memory_order_acquire))
      break;
  }
  next_queue_ = (start + 1) % queue_count;
  return progressed;
}

void QueuePoller::Run() {
  while (!start_gate_.load(std::memory_order_acquire))
    CpuRelax();

  std::uint64_t idle_rounds = 0;
  while (running_.load(std::memory_order_acquire)) {
    scans_.fetch_add(1, std::memory_order_relaxed);
    if (ScanOnce()) {
      idle_rounds = 0;
      continue;
    }

    empty_scans_.fetch_add(1, std::memory_order_relaxed);
    ++idle_rounds;
    if (idle_rounds <= options_.spin_rounds) {
      CpuRelax();
    } else if (idle_rounds <= static_cast<std::uint64_t>(options_.spin_rounds) +
                                  options_.yield_rounds) {
      yields_.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::yield();
    } else {
      sleeps_.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(
          std::chrono::microseconds(options_.idle_sleep_us));
    }
  }
}

} // namespace cxloom::loommem
