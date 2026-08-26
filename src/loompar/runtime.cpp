#include "cxloom/loompar/runtime.h"

#include "cxloom/common/tracing.h"

namespace cxloom::loompar {

LoomParRuntime::LoomParRuntime(CxloomConfig config, loommem::LoomMemRuntime* loommem)
    : config_(std::move(config)),
      loommem_(loommem),
      thread_manager_(config_.local_host_id),
      scheduler_(config_, loommem_) {}

Status LoomParRuntime::Initialize() {
    if (loommem_ == nullptr) {
        return Status::InvalidArgument("LoomPar requires a valid LoomMem runtime");
    }
    initialized_ = true;
    Trace("loompar", "initialized distributed thread runtime skeleton");
    return Status::Ok();
}

Status LoomParRuntime::Finalize() {
    initialized_ = false;
    return Status::Ok();
}

Result<std::uint64_t> LoomParRuntime::RegisterFunction(const std::string& name, ThreadFunction function) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomPar runtime must be initialized before registration");
    }
    return function_registry_.Register(name, function);
}

Result<GlobalThreadId> LoomParRuntime::CreateThread(const std::string& function_name,
                                                    std::vector<std::byte> arg_bytes,
                                                    const ThreadPlacementHint& hint) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomPar runtime must be initialized before thread creation");
    }

    auto function_id = function_registry_.Register(function_name, nullptr);
    if (!function_id.ok()) {
        return function_id.status();
    }

    auto target = scheduler_.SelectHost(hint, BuildLocalLoadView());
    if (!target.ok()) {
        return target.status();
    }

    auto gtid = thread_manager_.AllocateThread(target.value(), function_id.value(), std::move(arg_bytes));
    if (!gtid.ok()) {
        return gtid.status();
    }

    if (target.value() == config_.local_host_id) {
        thread_manager_.MarkRunning(gtid.value());
        Trace("loompar", "created local skeleton thread");
        return gtid.value();
    }

    auto launching_status = thread_manager_.MarkLaunching(gtid.value());
    if (!launching_status.ok()) {
        return launching_status;
    }

    return Status::Unimplemented("remote create path over CXL queues is not implemented yet");
}

Status LoomParRuntime::JoinThread(const GlobalThreadId& gtid) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomPar runtime must be initialized before join");
    }

    auto record = thread_manager_.Find(gtid);
    if (!record.ok()) {
        return record.status();
    }

    if (record.value()->state == ThreadState::kCompleted || record.value()->state == ThreadState::kRunning) {
        return thread_manager_.MarkJoined(gtid);
    }

    return Status::Unimplemented("blocking distributed join path is not implemented yet");
}

Status LoomParRuntime::Barrier(std::uint64_t barrier_id, std::size_t local_participants) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomPar runtime must be initialized before barrier");
    }

    auto query = barrier_manager_.Query(barrier_id);
    if (!query.ok()) {
        auto reg = barrier_manager_.RegisterBarrier(barrier_id, local_participants);
        if (!reg.ok()) {
            return reg;
        }
    }

    auto arrive = barrier_manager_.Arrive(barrier_id);
    if (!arrive.ok()) {
        return arrive;
    }

    return Status::Unimplemented("distributed barrier release path is not implemented yet");
}

std::vector<HostLoadSnapshot> LoomParRuntime::BuildLocalLoadView() const {
    std::vector<HostLoadSnapshot> view;
    view.reserve(config_.host_count);
    for (HostId host = 0; host < config_.host_count; ++host) {
        view.push_back(HostLoadSnapshot{host, host == config_.local_host_id ? 1U : 0U, 0U});
    }
    return view;
}

}  // namespace cxloom::loompar

