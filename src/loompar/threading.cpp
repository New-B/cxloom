#include "cxloom/loompar/threading.h"

#include <algorithm>


namespace cxloom::loompar {

namespace {
thread_local GlobalThreadId current_gtid{};
thread_local ThreadManager* current_manager = nullptr;
thread_local std::uint64_t* current_result = nullptr;
}

ThreadManager::ThreadManager(HostId local_host) : local_host_(local_host) {
    const unsigned count = std::min<unsigned>(4, std::max<unsigned>(2, std::thread::hardware_concurrency()));
    for (unsigned i = 0; i < count; ++i) workers_.emplace_back(&ThreadManager::WorkerLoop, this, i);
}

ThreadManager::~ThreadManager() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_cv_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
}

ExecutionLoad ThreadManager::SampleExecutionLoad() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ExecutionLoad load;
    load.workers = static_cast<std::uint32_t>(workers_.size());
    load.executing = executing_;
    // Only queued fibers are inspected: a running fiber changes wait state
    // outside this mutex. Ready-list membership provides the handoff boundary.
    for (const auto& entry : ready_) {
        if (entry->fiber->runnable()) ++load.ready;
        else ++load.blocked;
    }
    return load;
}

void ThreadManager::WorkerLoop(unsigned worker) {
    for (;;) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                auto next = std::find_if(ready_.begin(), ready_.end(), [&](const auto& candidate) {
                    return (candidate->worker < 0 || candidate->worker == static_cast<int>(worker)) &&
                           candidate->fiber->runnable();
                });
                if (next != ready_.end()) {
                    entry = *next;
                    ready_.erase(next);
                    entry->worker = static_cast<int>(worker);
                    entry->fiber->TransferOwnership(worker);
                    entry->queued = false;
                    ++executing_;
                    break;
                }
                if (stopping_ && ready_.empty()) return;
                // Parked contexts remain pinned. Poll deadlines/notifications at
                // a bounded interval without consuming a worker in the callback.
                if (ready_.empty()) ready_cv_.wait(lock);
                else ready_cv_.wait_for(lock, std::chrono::milliseconds(1));
            }
        }
        current_gtid = entry->record.gtid;
        current_manager = this;
        current_result = &entry->result;
        auto resumed = entry->fiber->Resume();
        current_result = nullptr;
        current_manager = nullptr;
        current_gtid = {};
        if (!resumed.ok()) entry->exit_code = -1;
        if (entry->fiber->done() || !resumed.ok()) {
            // Completion is observable only after leaving the stack. In
            // particular a detached record must not destroy an executing fiber.
            entry->fiber.reset();
            { std::lock_guard<std::mutex> lock(mutex_); --executing_; }
            if (entry->on_complete) entry->on_complete();
            MarkCompleted(entry->record.gtid, entry->exit_code, entry->result);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            --executing_;
            if (entry->record.migration_state == MigrationState::kQuiesced)
                entry->record.migration_state = MigrationState::kResumed;
            entry->queued = true;
            ready_.push_back(entry);
            ready_cv_.notify_all();
        }
    }
}

Result<GlobalThreadId> ThreadManager::AllocateThread(HostId execution_host, std::uint64_t function_id,
                                                     std::vector<std::byte> arg_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    const GlobalThreadId gtid{local_host_, next_tid_++};
    auto entry = std::make_shared<Entry>();
    entry->record = {gtid, execution_host, ThreadState::kAllocated, function_id, std::move(arg_bytes), 0, 0, 0, {}, MigrationState::kPinned, execution_host};
    records_.emplace(gtid.local_tid, std::move(entry));
    return gtid;
}

Status ThreadManager::MarkLaunching(const GlobalThreadId& gtid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    if (it->second->record.state != ThreadState::kAllocated)
        return Status::FailedPrecondition("thread is already launched");
    it->second->record.state = ThreadState::kLaunching;
    return Status::Ok();
}

Status ThreadManager::MarkRunning(const GlobalThreadId& gtid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    auto& state = it->second->record.state;
    if (state == ThreadState::kCompleted) return Status::Ok(); // A delayed ACK cannot undo completion.
    if (state != ThreadState::kLaunching && state != ThreadState::kRunning)
        return Status::FailedPrecondition("thread is not launching");
    state = ThreadState::kRunning;
    return Status::Ok();
}

Status ThreadManager::MarkCompleted(const GlobalThreadId& gtid, std::int32_t exit_code, std::uint64_t result_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    auto& entry = *it->second;
    if (entry.record.state == ThreadState::kCompleted) return Status::Ok();
    if (entry.record.state != ThreadState::kLaunching && entry.record.state != ThreadState::kRunning)
        return Status::FailedPrecondition("thread is not active");
    entry.record.exit_code = exit_code;
    entry.record.result_value = result_value;
    entry.record.state = ThreadState::kCompleted;
    entry.completed.notify_all();
    if (entry.detached) records_.erase(it);
    return Status::Ok();
}

Status ThreadManager::Launch(const GlobalThreadId& gtid, ThreadFunction function,
                             std::function<Status()> acquire, std::function<Status()> release,
                             std::function<std::uint64_t()> result,
                             std::function<std::vector<std::byte>()> result_bytes,
                             std::function<void()> on_complete) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    auto entry = it->second;
    if (!function || entry->record.state != ThreadState::kLaunching)
        return Status::FailedPrecondition("launch requires a registered function and launching thread");
    try {
        entry->on_complete = std::move(on_complete);
        entry->fiber = std::make_unique<Fiber>(64 * 1024, [this, entry = entry.get(), args = std::move(entry->record.arg_bytes), function, acquire = std::move(acquire), release = std::move(release), result = std::move(result), result_bytes = std::move(result_bytes)]() mutable {
            std::int32_t code = 0;
            try {
                if (acquire && !acquire().ok()) code = -1;
                if (!code) function(args.empty() ? nullptr : args.data());
                if (!code && result) entry->result = result();
                if (!code && result_bytes) {
                    auto bytes = result_bytes();
                    std::lock_guard<std::mutex> guard(mutex_);
                    entry->record.result_bytes = std::move(bytes);
                }
            }
            catch (...) { code = -1; }
            try { if (release && !release().ok()) code = -1; }
            catch (...) { code = -1; }
            entry->exit_code = code;
        });
        entry->record.state = ThreadState::kRunning;
        entry->queued = true;
        ready_.push_back(entry);
        ready_cv_.notify_all();
    } catch (const std::exception& error) {
        entry->record.state = ThreadState::kCompleted;
        entry->record.exit_code = -1;
        entry->completed.notify_all();
        return Status::Unavailable(error.what());
    }
    return Status::Ok();
}

Status ThreadManager::MigrationSafePoint(const GlobalThreadId& gtid) {
    Fiber* fiber = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(gtid.local_tid);
        if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown thread");
        if (current_manager != this || current_gtid.home_host != gtid.home_host || current_gtid.local_tid != gtid.local_tid ||
            Fiber::Current() != it->second->fiber.get())
            return Status::FailedPrecondition("safe point must be called by the running fiber");
        if (it->second->record.migration_state == MigrationState::kRequested)
            it->second->record.migration_state = MigrationState::kQuiesced;
        fiber = it->second->fiber.get();
    }
    fiber->SafePoint();
    return Status::Ok();
}

Status ThreadManager::RequestMigration(const GlobalThreadId& gtid, HostId target_host) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown thread");
    auto& record = it->second->record;
    if (record.state != ThreadState::kRunning || record.migration_state != MigrationState::kPinned)
        return Status::FailedPrecondition("thread is not migratable in its current state");
    record.migration_target = target_host;
    record.migration_epoch++;
    record.migration_state = MigrationState::kRequested;
    return Status::Ok();
}

GlobalThreadId ThreadManager::CurrentGtid() { return current_gtid; }
void ThreadManager::SetCurrentResult(std::uint64_t value) {
    if (current_result) *current_result = value;
}
Status ThreadManager::CurrentSafePoint() {
    if (!current_manager) return Status::FailedPrecondition("safe point requires a LoomPar callback");
    return current_manager->MigrationSafePoint(current_gtid);
}

Status ThreadManager::Join(const GlobalThreadId& gtid, std::function<Status()> acquire, std::uint64_t* result) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    auto entry = it->second;
    if (current_manager == this && current_gtid == gtid)
        return Status::FailedPrecondition("thread cannot join itself");
    if (entry->joining || entry->detached) return Status::FailedPrecondition("thread is joined or detached");
    if (entry->record.state == ThreadState::kAllocated)
        return Status::FailedPrecondition("thread has not launched");
    entry->joining = true;
    entry->completed.wait(lock, [&] { return entry->record.state == ThreadState::kCompleted; });
    lock.unlock();
    Status acquire_status;
    try { if (acquire) acquire_status = acquire(); }
    catch (...) { acquire_status = Status::Internal("join acquire hook failed"); }
    lock.lock();
    if (result) *result = entry->record.result_value;
    entry->record.state = ThreadState::kJoined;
    records_.erase(gtid.local_tid);
    if (!acquire_status.ok()) return acquire_status;
    return entry->record.exit_code == 0 ? Status::Ok() : Status::Internal("thread execution or remote launch failed");
}

Status ThreadManager::MarkJoined(const GlobalThreadId& gtid) { return Join(gtid); }

Status ThreadManager::Detach(const GlobalThreadId& gtid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    auto entry = it->second;
    if (entry->record.state == ThreadState::kAllocated) return Status::FailedPrecondition("thread has not launched");
    if (entry->joining || entry->detached) return Status::FailedPrecondition("thread is joined or detached");
    entry->detached = true;
    if (entry->record.state == ThreadState::kCompleted) records_.erase(it);
    return Status::Ok();
}

Result<ThreadRecord> ThreadManager::Find(const GlobalThreadId& gtid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    return it->second->record;
}

std::size_t ThreadManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

Result<std::uint64_t> FunctionRegistry::Register(const std::string& name, ThreadFunction function) {
    if (name.empty() || !function) return Status::InvalidArgument("function registration requires a name and function");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = by_name_.find(name);
    if (it != by_name_.end()) {
        // The same symbol may be represented by a different process-local
        // callable object. The stable name/ID is the cluster-wide binding.
        auto existing = by_id_.at(it->second);
        auto old_ptr = existing.target<void (*)(void*)>();
        auto new_ptr = function.target<void (*)(void*)>();
        if (old_ptr && new_ptr && *old_ptr != *new_ptr)
            return Status::AlreadyExists("function name has a different binding");
        return it->second;
    }
    // Stable across processes and independent of registration order (FNV-1a).
    std::uint64_t id = 14695981039346656037ULL;
    for (unsigned char c : name) { id ^= c; id *= 1099511628211ULL; }
    if (by_id_.count(id)) return Status::AlreadyExists("function ID collision");
    by_name_[name] = id;
    by_id_[id] = function;
    return id;
}

Result<std::uint64_t> FunctionRegistry::Lookup(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return Status::NotFound("unregistered function name");
    return it->second;
}

Result<ThreadFunction> FunctionRegistry::Resolve(std::uint64_t function_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = by_id_.find(function_id);
    if (it == by_id_.end()) return Status::NotFound("unknown function id");
    return it->second;
}

}  // namespace cxloom::loompar
