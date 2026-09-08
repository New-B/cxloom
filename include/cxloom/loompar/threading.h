#pragma once

#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>

#include "cxloom/common/status.h"
#include "cxloom/common/types.h"
#include "cxloom/loompar/fiber.h"

namespace cxloom::loompar {

enum class ThreadState : std::uint8_t {
    kAllocated = 0,
    kLaunching,
    kRunning,
    kCompleted,
    kJoined,
};
enum class MigrationState : std::uint8_t { kPinned = 0, kRequested, kQuiesced, kResumed, kAborted };

struct ThreadRecord {
    GlobalThreadId gtid {};
    HostId execution_host {0};
    ThreadState state {ThreadState::kAllocated};
    std::uint64_t function_id {0};
    std::vector<std::byte> arg_bytes;
    std::int32_t exit_code {0};
    std::uint64_t result_value {0};
    std::uint64_t migration_epoch {0};
    std::vector<std::byte> result_bytes;
    MigrationState migration_state {MigrationState::kPinned};
    HostId migration_target {0};
};

using ThreadFunction = std::function<void(void*)>;

class ThreadManager {
public:
    explicit ThreadManager(HostId local_host);
    ~ThreadManager();

    Result<GlobalThreadId> AllocateThread(HostId execution_host, std::uint64_t function_id,
                                          std::vector<std::byte> arg_bytes);
    Status MarkLaunching(const GlobalThreadId& gtid);
    Status MarkRunning(const GlobalThreadId& gtid);
    Status MarkCompleted(const GlobalThreadId& gtid, std::int32_t exit_code, std::uint64_t result_value = 0);
    Status MarkJoined(const GlobalThreadId& gtid);
    Status Join(const GlobalThreadId& gtid, std::function<Status()> acquire = {});
    Status Detach(const GlobalThreadId& gtid);
    Status Launch(const GlobalThreadId& gtid, ThreadFunction function,
                  std::function<Status()> acquire = {}, std::function<Status()> release = {},
                  std::function<std::uint64_t()> result = {},
                  std::function<std::vector<std::byte>()> result_bytes = {});
    std::size_t size() const;
    Result<ThreadRecord> Find(const GlobalThreadId& gtid);
    Status MigrationSafePoint(const GlobalThreadId& gtid);
    Status RequestMigration(const GlobalThreadId& gtid, HostId target_host);
    static GlobalThreadId CurrentGtid();

private:
    HostId local_host_ {0};
    ThreadLocalId next_tid_ {0};
    struct Entry {
        ThreadRecord record;
        std::unique_ptr<Fiber> fiber;
        std::condition_variable completed;
        bool joining {false};
        bool queued {false};
    };
    mutable std::mutex mutex_;
    std::unordered_map<ThreadLocalId, std::shared_ptr<Entry>> records_;
    std::deque<std::shared_ptr<Entry>> ready_;
    std::condition_variable ready_cv_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
    void WorkerLoop();
};

class FunctionRegistry {
public:
    Result<std::uint64_t> Register(const std::string& name, ThreadFunction function);
    Result<std::uint64_t> Lookup(const std::string& name) const;
    Result<ThreadFunction> Resolve(std::uint64_t function_id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, ThreadFunction> by_id_;
    std::unordered_map<std::string, std::uint64_t> by_name_;
};

}  // namespace cxloom::loompar
