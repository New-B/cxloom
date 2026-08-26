#pragma once

#include <vector>

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

    Status Initialize();
    Status Finalize();

    Result<std::uint64_t> RegisterFunction(const std::string& name, ThreadFunction function);
    Result<GlobalThreadId> CreateThread(const std::string& function_name,
                                        std::vector<std::byte> arg_bytes,
                                        const ThreadPlacementHint& hint);
    Status JoinThread(const GlobalThreadId& gtid);
    Status Barrier(std::uint64_t barrier_id, std::size_t local_participants);

    ThreadManager& thread_manager() { return thread_manager_; }
    FunctionRegistry& function_registry() { return function_registry_; }
    PlacementScheduler& scheduler() { return scheduler_; }

private:
    std::vector<HostLoadSnapshot> BuildLocalLoadView() const;

    CxloomConfig config_;
    loommem::LoomMemRuntime* loommem_ {nullptr};
    ThreadManager thread_manager_;
    FunctionRegistry function_registry_;
    PlacementScheduler scheduler_;
    BarrierManager barrier_manager_;
    bool initialized_ {false};
};

}  // namespace cxloom::loompar

