#pragma once
#include <algorithm>
#include <condition_variable>
#include <deque>
#include "cxloom/common/execution_context.h"

namespace cxloom {
// One waiter queue for native callers and fibers. Register before releasing the
// application lock; notifications select waiters, never a shared sequence value.
class CooperativeCondition {
public:
    template<class Predicate>
    void wait(std::unique_lock<std::mutex>& lock, Predicate predicate) {
        while (!predicate()) wait_until(lock, std::chrono::steady_clock::time_point::max());
    }
    void wait(std::unique_lock<std::mutex>& lock) {
        wait_until(lock, std::chrono::steady_clock::time_point::max());
    }
    std::cv_status wait_until(std::unique_lock<std::mutex>& lock,
                               std::chrono::steady_clock::time_point deadline) {
        auto state = std::make_shared<ExecutionWaitState>();
        state->deadline = deadline;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            waiters_.push_back(state);
        }
        lock.unlock();
        if (park_execution) park_execution(state);
        else {
            std::unique_lock<std::mutex> guard(mutex_);
            cv_.wait_until(guard, deadline, [&] { return state->ready.load(); });
        }
        bool signaled;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            signaled = state->ready.load();
            waiters_.erase(std::find(waiters_.begin(), waiters_.end(), state));
        }
        lock.lock();
        return signaled ? std::cv_status::no_timeout : std::cv_status::timeout;
    }
    template<class Rep, class Period, class Predicate>
    bool wait_for(std::unique_lock<std::mutex>& lock, std::chrono::duration<Rep, Period> duration, Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (!predicate()) {
            if (wait_until(lock, deadline) == std::cv_status::timeout) return predicate();
        }
        return true;
    }
    void notify_one() { Notify(false); }
    void notify_all() { Notify(true); }
private:
    void Notify(bool all) {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& state : waiters_) {
            if (state->ready.load() || std::chrono::steady_clock::now() >= state->deadline) continue;
            state->ready.store(true);
            if (!all) break;
        }
        cv_.notify_all();
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<ExecutionWaitState>> waiters_;
};
}  // namespace cxloom
