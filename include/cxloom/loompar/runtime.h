#pragma once

#include <vector>
#include <deque>
#include <atomic>
#include <unordered_map>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"
#include "cxloom/common/types.h"
#include "cxloom/loommem/runtime.h"
#include "cxloom/loompar/barrier.h"
#include "cxloom/loompar/scheduler.h"
#include "cxloom/loompar/threading.h"

namespace cxloom::loompar {

class LoomParRuntime {
public:
    LoomParRuntime(CxloomConfig config, loommem::LoomMemRuntime* loommem);

    ~LoomParRuntime();
    Status Initialize();
    Status Finalize();

    Result<std::uint64_t> RegisterFunction(const std::string& name, ThreadFunction function);
    Result<GlobalThreadId> CreateThread(const std::string& function_name,
                                        std::vector<std::byte> arg_bytes,
                                        const ThreadPlacementHint& hint,
                                        std::function<std::uint64_t()> result = {});
    Status JoinThread(const GlobalThreadId& gtid);
    // All configured hosts participate. Count is fixed per host and barrier ID.
    Status Barrier(std::uint64_t barrier_id, std::size_t local_participants);

    ThreadManager& thread_manager() { return thread_manager_; }
    FunctionRegistry& function_registry() { return function_registry_; }
    PlacementScheduler& scheduler() { return scheduler_; }

private:
    std::vector<HostLoadSnapshot> BuildLocalLoadView() const;
    Status HandleMessage(loommem::QueueEnvelope message);
    void Progress();
    void Enqueue(loommem::QueueEnvelope message);
    Status PublishLoad(HostId destination);
    Status ReserveExecution(HostId host);
    void ReleaseExecution(HostId host);
    struct RemoteExecution { GlobalThreadId home; GlobalThreadId local; };
    ThreadManager executions_;
    std::vector<RemoteExecution> remote_executions_;
    std::mutex control_mutex_;
    std::mutex api_mutex_;
    std::deque<loommem::QueueEnvelope> outgoing_;
    std::thread progress_;
    std::atomic<bool> stopping_ {false};

    CxloomConfig config_;
    loommem::LoomMemRuntime* loommem_ {nullptr};
    ThreadManager thread_manager_;
    FunctionRegistry function_registry_;
    PlacementScheduler scheduler_;
    BarrierManager barrier_manager_;
    std::atomic<std::size_t> active_barriers_ {0};
    std::atomic<std::uint32_t> local_running_ {0};
    std::atomic<std::uint32_t> local_pending_ {0};
    std::atomic<std::uint64_t> load_sequence_ {0};
    mutable std::mutex load_mutex_;
    std::unordered_map<HostId, HostLoadSnapshot> remote_load_;
    std::atomic<bool> initialized_ {false};
};

}  // namespace cxloom::loompar
