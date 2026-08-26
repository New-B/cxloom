#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cxloom/common/status.h"
#include "cxloom/common/types.h"

namespace cxloom::loompar {

enum class ThreadState : std::uint8_t {
    kAllocated = 0,
    kLaunching,
    kRunning,
    kCompleted,
    kJoined,
};

struct ThreadRecord {
    GlobalThreadId gtid {};
    HostId execution_host {0};
    ThreadState state {ThreadState::kAllocated};
    std::uint64_t function_id {0};
    std::vector<std::byte> arg_bytes;
    std::int32_t exit_code {0};
};

using ThreadFunction = void (*)(void*);

class ThreadManager {
public:
    explicit ThreadManager(HostId local_host);

    Result<GlobalThreadId> AllocateThread(HostId execution_host, std::uint64_t function_id,
                                          std::vector<std::byte> arg_bytes);
    Status MarkLaunching(const GlobalThreadId& gtid);
    Status MarkRunning(const GlobalThreadId& gtid);
    Status MarkCompleted(const GlobalThreadId& gtid, std::int32_t exit_code);
    Status MarkJoined(const GlobalThreadId& gtid);
    Result<ThreadRecord*> Find(const GlobalThreadId& gtid);

private:
    HostId local_host_ {0};
    ThreadLocalId next_tid_ {0};
    std::unordered_map<ThreadLocalId, ThreadRecord> records_;
};

class FunctionRegistry {
public:
    Result<std::uint64_t> Register(const std::string& name, ThreadFunction function);
    Result<ThreadFunction> Resolve(std::uint64_t function_id) const;

private:
    std::uint64_t next_function_id_ {1};
    std::unordered_map<std::uint64_t, ThreadFunction> by_id_;
    std::unordered_map<std::string, std::uint64_t> by_name_;
};

}  // namespace cxloom::loompar

