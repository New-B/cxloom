#include "cxloom/loompar/threading.h"

#include <algorithm>


namespace cxloom::loompar {

namespace {
thread_local GlobalThreadId current_gtid{};
}

ThreadManager::ThreadManager(HostId local_host) : local_host_(local_host) {
    const unsigned count = std::min<unsigned>(4, std::max<unsigned>(2, std::thread::hardware_concurrency()));
    for (unsigned i = 0; i < count; ++i) workers_.emplace_back(&ThreadManager::WorkerLoop, this);
}

ThreadManager::~ThreadManager() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_cv_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    std::vector<std::shared_ptr<Entry>> entries;
    { std::lock_guard<std::mutex> lock(mutex_);
      for (auto& item : records_) entries.push_back(item.second); }
}

void ThreadManager::WorkerLoop() {
    for (;;) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_cv_.wait(lock, [&] { return stopping_ || !ready_.empty(); });
            if (stopping_ && ready_.empty()) return;
            entry = std::move(ready_.front());
            ready_.pop_front();
            entry->queued = false;
        }
        if (!entry->fiber || entry->fiber->done()) continue;
        entry->fiber->Resume();
        std::lock_guard<std::mutex> lock(mutex_);
        if (entry->record.migration_state == MigrationState::kQuiesced)
            entry->record.migration_state = MigrationState::kResumed;
        if (!entry->fiber->done() && !stopping_ && !entry->queued) {
            entry->queued = true;
            ready_.push_back(entry);
            ready_cv_.notify_one();
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
    return Status::Ok();
}

Status ThreadManager::Launch(const GlobalThreadId& gtid, ThreadFunction function,
                             std::function<Status()> acquire, std::function<Status()> release,
                             std::function<std::uint64_t()> result,
                             std::function<std::vector<std::byte>()> result_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    auto entry = it->second;
    if (!function || entry->record.state != ThreadState::kLaunching)
        return Status::FailedPrecondition("launch requires a registered function and launching thread");
    try {
        entry->fiber = std::make_unique<Fiber>(64 * 1024, [this, entry, args = std::move(entry->record.arg_bytes), function, gtid, acquire = std::move(acquire), release = std::move(release), result = std::move(result), result_bytes = std::move(result_bytes)]() mutable {
            current_gtid = gtid;
            std::int32_t code = 0;
            try {
                if (acquire && !acquire().ok()) code = -1;
            if (!code) function(args.empty() ? nullptr : args.data());
            if (!code && result) entry->record.result_value = result();
            if (!code && result_bytes) entry->record.result_bytes = result_bytes();
            }
            catch (...) { code = -1; }
            try { if (release && !release().ok()) code = -1; }
            catch (...) { code = -1; }
            MarkCompleted(gtid, code);
            current_gtid = {};
        });
        entry->record.state = ThreadState::kRunning;
        entry->queued = true;
        ready_.push_back(entry);
        ready_cv_.notify_one();
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
        if (current_gtid.home_host != gtid.home_host || current_gtid.local_tid != gtid.local_tid ||
            Fiber::Current() != it->second->fiber.get())
            return Status::FailedPrecondition("safe point must be called by the running fiber");
        it->second->record.migration_epoch++;
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

Status ThreadManager::Join(const GlobalThreadId& gtid, std::function<Status()> acquire) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = records_.find(gtid.local_tid);
    if (gtid.home_host != local_host_ || it == records_.end()) return Status::NotFound("unknown home thread");
    auto entry = it->second;
    if (entry->fiber && Fiber::Current() == entry->fiber.get())
        return Status::FailedPrecondition("thread cannot join itself");
    if (entry->joining) return Status::FailedPrecondition("thread already has a joiner");
    if (entry->record.state == ThreadState::kAllocated)
        return Status::FailedPrecondition("thread has not launched");
    entry->joining = true;
    entry->completed.wait(lock, [&] { return entry->record.state == ThreadState::kCompleted; });
    lock.unlock();
    Status acquire_status;
    try { if (acquire) acquire_status = acquire(); }
    catch (...) { acquire_status = Status::Internal("join acquire hook failed"); }
    lock.lock();
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
    if (entry->joining) return Status::FailedPrecondition("thread is already being joined");
    records_.erase(it);
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
