#include "cxloom/loompar/runtime.h"

#include "cxloom/common/tracing.h"
#include <cstring>
#include <chrono>

namespace cxloom::loompar {
namespace {
constexpr std::uint32_t kWireMagic = 0x43584c50; // "CXLP"
constexpr std::uint16_t kWireVersion = 1;
struct BarrierWire {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t reserved;
    std::uint64_t id;
    std::uint64_t generation;
    std::uint64_t failed;
};
loommem::QueueEnvelope EncodeBarrier(MessageKind kind, HostId source, HostId destination,
                                     std::uint64_t id, std::uint64_t generation, bool failed) {
    BarrierWire wire{kWireMagic, kWireVersion, 0, id, generation, failed ? 1ULL : 0ULL};
    loommem::QueueEnvelope message;
    message.header = {kind, source, destination, sizeof(wire)};
    message.payload.resize(sizeof(wire));
    std::memcpy(message.payload.data(), &wire, sizeof(wire));
    return message;
}
struct LoadWire { std::uint32_t magic; std::uint16_t version; std::uint16_t reserved0; std::uint32_t running; std::uint32_t pending; std::uint32_t queued; std::uint32_t reserved; std::uint64_t sequence; std::uint64_t sampled_at_ns; };
loommem::QueueEnvelope EncodeLoad(HostId source, HostId destination,
                                  std::uint32_t running, std::uint32_t pending,
                                  std::uint32_t queued, std::uint64_t sequence, std::uint64_t sampled_at_ns) {
    LoadWire wire{kWireMagic, kWireVersion, 0, running, pending, queued, 0, sequence, sampled_at_ns};
    loommem::QueueEnvelope message;
    message.header = {MessageKind::kLoadUpdate, source, destination, sizeof(wire)};
    message.payload.resize(sizeof(wire));
    std::memcpy(message.payload.data(), &wire, sizeof(wire));
    return message;
}
}


LoomParRuntime::LoomParRuntime(CxloomConfig config, loommem::LoomMemRuntime* loommem)
    : executions_(config.local_host_id),
      config_(std::move(config)),
      loommem_(loommem),
      thread_manager_(config_.local_host_id),
      scheduler_(config_, loommem_),
      barrier_manager_(config_.local_host_id, config_.host_count,
          [this](MessageKind kind, HostId target, std::uint64_t id, std::uint64_t generation, bool failed) {
              Enqueue(EncodeBarrier(kind, config_.local_host_id, target, id, generation, failed));
          }) {}

namespace {
// Fixed-width wire prefix; no virtual addresses or STL objects cross the queue.
struct ThreadWire {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t reserved;
    std::uint64_t tid;
    std::uint64_t function;
    std::int32_t code;
    std::uint16_t home;
    std::uint16_t argument_bytes;
    std::uint64_t result_value;
    std::uint64_t migration_epoch;
};
loommem::QueueEnvelope Encode(MessageKind kind, HostId source, HostId destination,
                              GlobalThreadId id, std::uint64_t function = 0,
                              std::int32_t code = 0, const std::vector<std::byte>& args = {},
                              std::uint64_t result_value = 0, std::uint64_t migration_epoch = 0) {
    ThreadWire wire{kWireMagic, kWireVersion, 0, id.local_tid, function, code, id.home_host, static_cast<std::uint16_t>(args.size()), result_value, migration_epoch};
    loommem::QueueEnvelope message;
    message.header = {kind, source, destination, static_cast<std::uint32_t>(sizeof(wire) + args.size())};
    message.payload.resize(message.header.payload_bytes);
    std::memcpy(message.payload.data(), &wire, sizeof(wire));
    if (!args.empty()) std::memcpy(message.payload.data() + sizeof(wire), args.data(), args.size());
    return message;
}
}

LoomParRuntime::~LoomParRuntime() {
    // Applications must join their home threads and coordinate all hosts before finalize.
    stopping_ = true;
    if (progress_.joinable()) progress_.join();
    if (initialized_ && config_.host_count > 1) loommem_->StopQueuePoller();
}

Status LoomParRuntime::Initialize() {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (initialized_) return Status::AlreadyExists("LoomPar already initialized");
    if (loommem_ == nullptr) return Status::InvalidArgument("LoomPar requires LoomMem");
    if (config_.host_count == 0 || config_.local_host_id >= config_.host_count)
        return Status::InvalidArgument("invalid host configuration");
    if (config_.host_count > 1) {
        auto status = loommem_->StartQueuePoller([this](loommem::QueueEnvelope message) {
            return HandleMessage(std::move(message));
        });
        if (!status.ok()) return status;
        stopping_ = false;
        try { progress_ = std::thread(&LoomParRuntime::Progress, this); }
        catch (...) { loommem_->StopQueuePoller(); return Status::Unavailable("cannot start progress thread"); }
    }
    {
        std::lock_guard<std::mutex> load_lock(load_mutex_);
        for (HostId host = 0; host < config_.host_count; ++host)
            remote_load_[host] = HostLoadSnapshot{host, 0, 0};
    }
    initialized_ = true;
    return Status::Ok();
}

Status LoomParRuntime::Finalize() {
    std::lock_guard<std::mutex> api_lock(api_mutex_);
    if (!initialized_) return Status::Ok();
    if (active_barriers_ || !barrier_manager_.idle() || loommem_->staged_write_count())
        return Status::FailedPrecondition("finish barriers and publish staged writes before finalize");
    // Give the progress thread a short quiescence window to flush load and
    // completion telemetry produced by the final join.
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        bool empty = false;
        { std::lock_guard<std::mutex> lock(control_mutex_); empty = outgoing_.empty(); }
        if (empty) break;
        if (std::chrono::steady_clock::now() >= drain_deadline)
            return Status::FailedPrecondition("outgoing control messages did not drain before finalize");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (thread_manager_.size() || !remote_executions_.empty() || !outgoing_.empty())
            return Status::FailedPrecondition("join home threads and drain remote executions before finalize");
    }
    if (config_.host_count > 1) {
        auto status = loommem_->StopQueuePoller();
        stopping_ = true;
        if (progress_.joinable()) progress_.join();
        if (!status.ok()) return status;
    }
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
                                                    const ThreadPlacementHint& hint,
                                                    std::function<std::uint64_t()> result) {
    std::lock_guard<std::mutex> api_lock(api_mutex_);
    if (!initialized_) {
        return Status::FailedPrecondition("LoomPar runtime must be initialized before thread creation");
    }

    auto function_id = function_registry_.Lookup(function_name);
    if (!function_id.ok()) {
        return function_id.status();
    }

    auto target = scheduler_.SelectHost(hint, BuildLocalLoadView());
    if (!target.ok()) {
        return target.status();
    }

    if (target.value() >= config_.host_count) return Status::InvalidArgument("execution host out of range");
    if (target.value() != config_.local_host_id && arg_bytes.size() > loommem::kQueuePayloadBytes - sizeof(ThreadWire))
        return Status::InvalidArgument("remote inline arguments exceed queue payload; pass a GPtr for larger data");
    auto function = function_registry_.Resolve(function_id.value());
    if (!function.ok()) return function.status();
    auto published = loommem_->SynchronizeRelease();
    if (!published.ok()) return published;
    auto gtid = thread_manager_.AllocateThread(target.value(), function_id.value(), arg_bytes);
    if (!gtid.ok()) return gtid.status();
    thread_manager_.MarkLaunching(gtid.value());
    if (target.value() == config_.local_host_id) {
        auto reserve = ReserveExecution(target.value());
        if (!reserve.ok()) {
            thread_manager_.MarkCompleted(gtid.value(), -2);
            thread_manager_.Join(gtid.value());
            return reserve;
        }
        scheduler_.RecordLaunch(target.value());
        auto status = thread_manager_.Launch(gtid.value(), function.value(),
            [this] { return loommem_->SynchronizeAcquire(); },
            [this] { return loommem_->SynchronizeRelease(); }, std::move(result));
        if (!status.ok()) { ReleaseExecution(target.value()); thread_manager_.Join(gtid.value()); return status; }
    } else {
        if (config_.max_pending_creates_per_host &&
            local_pending_.load(std::memory_order_acquire) >= config_.max_pending_creates_per_host)
            { thread_manager_.MarkCompleted(gtid.value(), -3); thread_manager_.Join(gtid.value());
              return Status::Unavailable("local pending-create limit reached"); }
        local_pending_.fetch_add(1, std::memory_order_acq_rel);
        scheduler_.RecordLaunch(target.value());
        for (HostId host = 0; host < config_.host_count; ++host) PublishLoad(host);
        Enqueue(Encode(MessageKind::kCreateReq, config_.local_host_id, target.value(),
                       gtid.value(), function_id.value(), 0, arg_bytes));
    }
    return gtid.value();
}

Status LoomParRuntime::JoinThread(const GlobalThreadId& gtid) {
    if (!initialized_) {
        return Status::FailedPrecondition("LoomPar runtime must be initialized before join");
    }

    auto record = thread_manager_.Find(gtid);
    if (!record.ok()) return record.status();
    auto status = thread_manager_.Join(gtid, [this] { return loommem_->SynchronizeAcquire(); });
    if (record.value().execution_host == config_.local_host_id) {
        ReleaseExecution(config_.local_host_id);
        scheduler_.RecordCompletion(config_.local_host_id);
        for (HostId host = 0; host < config_.host_count; ++host) PublishLoad(host);
    }
    return status;
}

void LoomParRuntime::Enqueue(loommem::QueueEnvelope message) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    outgoing_.push_back(std::move(message));
}

Status LoomParRuntime::ReserveExecution(HostId host) {
    if (host != config_.local_host_id) return Status::InvalidArgument("can only reserve local execution");
    const auto limit = config_.max_running_threads_per_host;
    auto current = local_running_.load(std::memory_order_acquire);
    for (;;) {
        if (limit && current >= limit) return Status::Unavailable("local running-thread limit reached");
        if (local_running_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel)) return Status::Ok();
    }
}

void LoomParRuntime::ReleaseExecution(HostId host) {
    if (host == config_.local_host_id) local_running_.fetch_sub(1, std::memory_order_acq_rel);
}

Status LoomParRuntime::PublishLoad(HostId destination) {
    if (destination == config_.local_host_id || config_.host_count == 1) return Status::Ok();
    std::uint32_t queued = 0;
    auto queue = loommem_->GetQueue(config_.local_host_id, destination);
    if (queue.ok()) { auto size = queue.value()->Size(); if (size.ok()) queued = static_cast<std::uint32_t>(size.value()); }
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    Enqueue(EncodeLoad(config_.local_host_id, destination,
                       local_running_.load(std::memory_order_acquire),
                       local_pending_.load(std::memory_order_acquire), queued,
                       load_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1, now));
    return Status::Ok();
}

Status LoomParRuntime::HandleMessage(loommem::QueueEnvelope message) {
    if (message.header.kind == MessageKind::kMigrateReq) {
        if (message.header.dst_host != config_.local_host_id || message.payload.size() != sizeof(MigrationRequest))
            return Status::InvalidArgument("invalid migration request");
        MigrationRequest request{};
        std::memcpy(&request, message.payload.data(), sizeof(request));
        MigrationAck ack{};
        ack.header = {MessageKind::kMigrateAck, config_.local_host_id, request.header.src_host,
                      static_cast<std::uint32_t>(sizeof(ack))};
        ack.gtid = request.gtid;
        ack.target_host = config_.local_host_id;
        ack.migration_epoch = request.migration_epoch;
        // Native std::thread execution contexts cannot be captured safely. The
        // handler is explicit and deterministic: callers receive a rejection
        // until a cooperative resumable context is attached to the record.
        ack.status = static_cast<std::int32_t>(StatusCode::kUnimplemented);
        loommem::QueueEnvelope envelope;
        envelope.header = ack.header;
        envelope.payload.resize(sizeof(ack));
        std::memcpy(envelope.payload.data(), &ack, sizeof(ack));
        Enqueue(std::move(envelope));
        return Status::Ok();
    }
    if (message.header.kind == MessageKind::kMigrateAck) {
        if (message.header.dst_host != config_.local_host_id || message.payload.size() != sizeof(MigrationAck))
            return Status::InvalidArgument("invalid migration acknowledgement");
        return Status::Ok();
    }
    if (message.header.kind == MessageKind::kLoadUpdate) {
        if (message.header.dst_host != config_.local_host_id || message.payload.size() != sizeof(LoadWire) ||
            message.header.src_host >= config_.host_count) return Status::InvalidArgument("invalid load update");
        LoadWire wire{};
        std::memcpy(&wire, message.payload.data(), sizeof(wire));
        if (wire.magic != kWireMagic || wire.version != kWireVersion)
            return Status::InvalidArgument("unsupported load-update protocol version");
        std::lock_guard<std::mutex> lock(load_mutex_);
        auto& sample = remote_load_[message.header.src_host];
        if (wire.sequence >= sample.sample_sequence)
            sample = HostLoadSnapshot{message.header.src_host, wire.running, wire.pending,
                                      wire.queued, wire.sequence, wire.sampled_at_ns};
        return Status::Ok();
    }
    if (message.header.kind == MessageKind::kBarrierArrive || message.header.kind == MessageKind::kBarrierRelease) {
        if (message.header.dst_host != config_.local_host_id || message.payload.size() != sizeof(BarrierWire) ||
            message.header.payload_bytes != sizeof(BarrierWire))
            return Status::InvalidArgument("invalid barrier envelope");
        BarrierWire wire{};
        std::memcpy(&wire, message.payload.data(), sizeof(wire));
        if (wire.magic != kWireMagic || wire.version != kWireVersion)
            return Status::InvalidArgument("unsupported barrier protocol version");
        if (wire.failed > 1) return Status::InvalidArgument("invalid barrier failure flag");
        return barrier_manager_.Handle(message.header.kind, message.header.src_host,
                                       wire.id, wire.generation, wire.failed != 0);
    }
    if (message.header.dst_host != config_.local_host_id || message.header.src_host >= config_.host_count ||
        message.payload.size() < sizeof(ThreadWire)) return Status::InvalidArgument("invalid thread message");
    ThreadWire wire{};
    std::memcpy(&wire, message.payload.data(), sizeof(wire));
    if (wire.magic != kWireMagic || wire.version != kWireVersion)
        return Status::InvalidArgument("unsupported thread protocol version");
    if (message.payload.size() != sizeof(wire) + wire.argument_bytes)
        return Status::InvalidArgument("invalid thread argument length");
    GlobalThreadId id{wire.home, wire.tid};
    if (message.header.kind == MessageKind::kCreateReq) {
        if (id.home_host != message.header.src_host) return Status::InvalidArgument("create home mismatch");
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            for (const auto& existing : remote_executions_)
                if (existing.home == id) return Status::Ok();
        }
        auto function = function_registry_.Resolve(wire.function);
        if (!function.ok()) {
            Enqueue(Encode(MessageKind::kCompleteNotify, config_.local_host_id, id.home_host, id, 0, -1));
            return Status::Ok();
        }
        std::vector<std::byte> args(message.payload.begin() + sizeof(wire), message.payload.end());
        auto reserve = ReserveExecution(config_.local_host_id);
        if (!reserve.ok()) {
            Enqueue(Encode(MessageKind::kCompleteNotify, config_.local_host_id, id.home_host, id, 0, -2));
            return Status::Ok();
        }
        auto local = executions_.AllocateThread(config_.local_host_id, wire.function, std::move(args));
        if (!local.ok()) { ReleaseExecution(config_.local_host_id); return local.status(); }
        executions_.MarkLaunching(local.value());
        auto status = executions_.Launch(local.value(), function.value(),
            [this] { return loommem_->SynchronizeAcquire(); },
            [this] { return loommem_->SynchronizeRelease(); });
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            remote_executions_.push_back({id, local.value()});
            if (status.ok()) outgoing_.push_back(Encode(MessageKind::kCreateAck, config_.local_host_id, id.home_host, id));
        }
        if (!status.ok()) ReleaseExecution(config_.local_host_id);
        return Status::Ok();
    }
    auto record = thread_manager_.Find(id);
    if (!record.ok()) return Status::Ok(); // Late acknowledgement after join.
    if (record.value().execution_host != message.header.src_host)
        return Status::InvalidArgument("completion execution host mismatch");
    if (message.header.kind == MessageKind::kCreateAck || message.header.kind == MessageKind::kCompleteNotify) {
        if (message.header.kind == MessageKind::kCompleteNotify &&
            record.value().migration_epoch != wire.migration_epoch)
            return Status::Ok();
        auto status = message.header.kind == MessageKind::kCreateAck
            ? thread_manager_.MarkRunning(id) : thread_manager_.MarkCompleted(id, wire.code, wire.result_value);
        if (message.header.kind == MessageKind::kCompleteNotify) {
            local_pending_.fetch_sub(1, std::memory_order_acq_rel);
            scheduler_.RecordCompletion(message.header.src_host);
        }
        if (message.header.kind == MessageKind::kCompleteNotify)
            for (HostId host = 0; host < config_.host_count; ++host) PublishLoad(host);
        // Join may reclaim the record between the snapshot and this transition.
        return status.code() == StatusCode::kNotFound ? Status::Ok() : status;
    }
    return Status::InvalidArgument("unsupported LoomPar message");
}

void LoomParRuntime::Progress() {
    while (!stopping_) {
        bool load_changed = false;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            for (auto it = remote_executions_.begin(); it != remote_executions_.end();) {
                auto record = executions_.Find(it->local);
                if (record.ok() && record.value().state == ThreadState::kCompleted) {
                    executions_.Join(it->local);
                    ReleaseExecution(config_.local_host_id);
                    load_changed = true;
                    outgoing_.push_back(Encode(MessageKind::kCompleteNotify, config_.local_host_id,
                                               it->home.home_host, it->home, 0, record.value().exit_code, {},
                                               record.value().result_value, record.value().migration_epoch));
                    it = remote_executions_.erase(it);
                } else ++it;
            }
            // Try once per pending message, so a full destination cannot block other hosts or the poller.
            const auto count = outgoing_.size();
            for (std::size_t i = 0; i < count; ++i) {
                auto message = std::move(outgoing_.front());
                outgoing_.pop_front();
                auto queue = loommem_->GetQueue(config_.local_host_id, message.header.dst_host);
                auto status = queue.ok() ? queue.value()->Push(message) : queue.status();
                if (!status.ok()) outgoing_.push_back(std::move(message));
            }
        }
        if (load_changed) {
            for (HostId host = 0; host < config_.host_count; ++host) PublishLoad(host);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

Status LoomParRuntime::Barrier(std::uint64_t barrier_id, std::size_t local_participants) {
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        if (!initialized_) return Status::FailedPrecondition("initialize LoomPar before barrier");
        if (!local_participants) return Status::InvalidArgument("barrier requires local participants");
        ++active_barriers_;
    }
    struct ActiveGuard {
        std::atomic<std::size_t>& count;
        ~ActiveGuard() { --count; }
    } active{active_barriers_};
    auto status = loommem_->SynchronizeRelease();
    if (!status.ok()) { barrier_manager_.Break(barrier_id); return status; }
    status = barrier_manager_.Wait(barrier_id, local_participants);
    if (!status.ok()) return status;
    status = loommem_->SynchronizeAcquire();
    if (!status.ok()) barrier_manager_.Break(barrier_id);
    return status;
}

std::vector<HostLoadSnapshot> LoomParRuntime::BuildLocalLoadView() const {
    std::vector<HostLoadSnapshot> view;
    view.reserve(config_.host_count);
    std::lock_guard<std::mutex> lock(load_mutex_);
    for (HostId host = 0; host < config_.host_count; ++host)
        view.push_back(host == config_.local_host_id
            ? HostLoadSnapshot{host, local_running_.load(), local_pending_.load(), 0, load_sequence_.load(), 0}
            : remote_load_.at(host));
    return view;
}

}  // namespace cxloom::loompar
