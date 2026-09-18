#pragma once
#include <atomic>
#include <cstdint>
#include <chrono>
#include <memory>

namespace cxloom {
struct ExecutionWaitState {
    std::atomic<bool> ready{false};
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
};
// Installed by a cooperative executor while running a context. Lower layers
// can suspend without depending on the LoomPar library.
inline thread_local void (*park_execution)(std::shared_ptr<ExecutionWaitState>) = nullptr;
// Process-local identity: native callers and fibers have disjoint lifetimes.
inline std::uint64_t NewExecutionContextId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
inline thread_local std::uint64_t active_execution_context = 0;
inline std::uint64_t CurrentExecutionContextId() {
    thread_local const auto native = NewExecutionContextId();
    return active_execution_context ? active_execution_context : native;
}
}  // namespace cxloom
