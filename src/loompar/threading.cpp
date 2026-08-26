#include "cxloom/loompar/threading.h"

namespace cxloom::loompar {

ThreadManager::ThreadManager(HostId local_host) : local_host_(local_host) {}

Result<GlobalThreadId> ThreadManager::AllocateThread(HostId execution_host, std::uint64_t function_id,
                                                     std::vector<std::byte> arg_bytes) {
    const GlobalThreadId gtid{local_host_, next_tid_++};
    records_.emplace(gtid.local_tid, ThreadRecord{gtid, execution_host, ThreadState::kAllocated, function_id,
                                                  std::move(arg_bytes), 0});
    return gtid;
}

Status ThreadManager::MarkLaunching(const GlobalThreadId& gtid) {
    auto it = records_.find(gtid.local_tid);
    if (it == records_.end()) {
        return Status::NotFound("unknown thread id");
    }
    it->second.state = ThreadState::kLaunching;
    return Status::Ok();
}

Status ThreadManager::MarkRunning(const GlobalThreadId& gtid) {
    auto it = records_.find(gtid.local_tid);
    if (it == records_.end()) {
        return Status::NotFound("unknown thread id");
    }
    it->second.state = ThreadState::kRunning;
    return Status::Ok();
}

Status ThreadManager::MarkCompleted(const GlobalThreadId& gtid, std::int32_t exit_code) {
    auto it = records_.find(gtid.local_tid);
    if (it == records_.end()) {
        return Status::NotFound("unknown thread id");
    }
    it->second.state = ThreadState::kCompleted;
    it->second.exit_code = exit_code;
    return Status::Ok();
}

Status ThreadManager::MarkJoined(const GlobalThreadId& gtid) {
    auto it = records_.find(gtid.local_tid);
    if (it == records_.end()) {
        return Status::NotFound("unknown thread id");
    }
    it->second.state = ThreadState::kJoined;
    return Status::Ok();
}

Result<ThreadRecord*> ThreadManager::Find(const GlobalThreadId& gtid) {
    auto it = records_.find(gtid.local_tid);
    if (it == records_.end()) {
        return Status::NotFound("unknown thread id");
    }
    return &it->second;
}

Result<std::uint64_t> FunctionRegistry::Register(const std::string& name, ThreadFunction function) {
    auto it = by_name_.find(name);
    if (it != by_name_.end()) {
        return it->second;
    }

    const auto function_id = next_function_id_++;
    by_name_[name] = function_id;
    by_id_[function_id] = function;
    return function_id;
}

Result<ThreadFunction> FunctionRegistry::Resolve(std::uint64_t function_id) const {
    auto it = by_id_.find(function_id);
    if (it == by_id_.end()) {
        return Status::NotFound("unknown function id");
    }
    return it->second;
}

}  // namespace cxloom::loompar

