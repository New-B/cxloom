#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ucontext.h>

#include "cxloom/common/status.h"

namespace cxloom::loompar {

// Cooperative user-space execution context. A Fiber owns its stack and can be
// suspended only at Yield/safe-point boundaries; it never attempts to capture
// an arbitrary native pthread stack.
class Fiber {
 public:
    struct Impl;
    using Entry = std::function<void()>;

    Fiber(std::size_t stack_bytes, Entry entry);
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    Status Resume();
    bool done() const { return done_; }
    bool at_safe_point() const { return safe_point_; }
    Status TransferOwnership(std::uint32_t worker_id);
    std::uint32_t owner() const { return owner_; }
    void SafePoint();
    static void Yield();
    static Fiber* Current();

 private:
    std::unique_ptr<Impl> impl_;
    bool done_{false};
    bool safe_point_{false};
    std::uint32_t owner_{0};
};

}  // namespace cxloom::loompar
