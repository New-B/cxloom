#include "cxloom/loompar/fiber.h"

#include <ucontext.h>
#include <cstdlib>

namespace cxloom::loompar {
struct Fiber::Impl {
    ucontext_t context{};
    ucontext_t caller{};
    std::unique_ptr<std::byte[]> stack;
    std::size_t stack_bytes{0};
    Entry entry;
    Fiber* owner{nullptr};
    bool done{false};
};

namespace {
thread_local Fiber::Impl* current = nullptr;
void Trampoline(std::uintptr_t raw) {
    auto* impl = reinterpret_cast<Fiber::Impl*>(raw);
    current = impl;
    impl->entry();
    impl->done = true;
    current = nullptr;
    setcontext(&impl->caller);
}
}

Fiber::Fiber(std::size_t stack_bytes, Entry entry) : impl_(std::make_unique<Impl>()) {
    impl_->stack_bytes = stack_bytes < 16384 ? 16384 : stack_bytes;
    impl_->stack = std::make_unique<std::byte[]>(impl_->stack_bytes);
    impl_->entry = std::move(entry);
    impl_->owner = this;
    getcontext(&impl_->context);
    impl_->context.uc_stack.ss_sp = impl_->stack.get();
    impl_->context.uc_stack.ss_size = impl_->stack_bytes;
    impl_->context.uc_link = nullptr;
    makecontext(&impl_->context, reinterpret_cast<void (*)()>(&Trampoline), 1,
                static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(impl_.get())));
}

Fiber::~Fiber() = default;

Status Fiber::TransferOwnership(std::uint32_t worker_id) {
    if (!safe_point_ && current != nullptr && current->owner == this)
        return Status::FailedPrecondition("fiber ownership can transfer only at a safe point");
    owner_ = worker_id;
    return Status::Ok();
}

Status Fiber::Resume() {
    if (impl_->done) return Status::FailedPrecondition("fiber has completed");
    safe_point_ = false;
    if (swapcontext(&impl_->caller, &impl_->context) != 0)
        return Status::Internal("fiber context switch failed");
    done_ = impl_->done;
    return Status::Ok();
}

void Fiber::SafePoint() {
    safe_point_ = true;
    Yield();
}

void Fiber::Yield() {
    if (current == nullptr) return;
    swapcontext(&current->context, &current->caller);
}

Fiber* Fiber::Current() { return current == nullptr ? nullptr : current->owner; }
}  // namespace cxloom::loompar
