#include "cxloom/loompar/barrier.h"

namespace cxloom::loompar {
BarrierManager::BarrierManager(HostId local_host, std::uint16_t host_count, Sender sender)
    : local_host_(local_host), host_count_(host_count), send_(std::move(sender)) {}

std::shared_ptr<BarrierManager::Entry> BarrierManager::Get(std::uint64_t id) {
    auto& entry = barriers_[id];
    if (!entry) { entry = std::make_shared<Entry>(); entry->state.barrier_id = id; }
    return entry;
}

void BarrierManager::AdvanceLocked(Entry& entry) {
    ++entry.state.generation;
    entry.state.local_arrivals = 0;
    entry.host_arrivals = 0;
    entry.reported = false;
    entry.ready.notify_all();
}

void BarrierManager::BreakLocked(Entry& entry) {
    if (entry.broken) return;
    entry.broken = true;
    entry.ready.notify_all();
    for (HostId host = 0; host < host_count_; ++host)
        if (host != local_host_ && send_)
            send_(MessageKind::kBarrierRelease, host, entry.state.barrier_id, entry.state.generation, true);
}

void BarrierManager::ArriveLocked(Entry& entry, HostId source) {
    entry.host_arrivals |= std::uint64_t{1} << source;
    const auto all = host_count_ == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << host_count_) - 1;
    if (entry.host_arrivals != all) return;
    for (HostId host = 1; host < host_count_; ++host)
        send_(MessageKind::kBarrierRelease, host, entry.state.barrier_id, entry.state.generation, false);
    AdvanceLocked(entry);
}

Status BarrierManager::Wait(std::uint64_t id, std::size_t participants) {
    if (!participants) return Status::InvalidArgument("barrier requires positive local participant count");
    if (!host_count_ || host_count_ > kMaxHosts || local_host_ >= host_count_ || (host_count_ > 1 && !send_))
        return Status::FailedPrecondition("invalid barrier world configuration");
    std::unique_lock<std::mutex> lock(mutex_);
    auto entry = Get(id);
    if (entry->broken) return Status::FailedPrecondition("barrier is broken");
    if (entry->state.local_participants && entry->state.local_participants != participants) {
        BreakLocked(*entry);
        return Status::InvalidArgument("local participant count cannot change for a barrier ID");
    }
    entry->state.local_participants = participants;
    if (entry->reported) {
        BreakLocked(*entry);
        return Status::FailedPrecondition("too many local barrier participants");
    }
    const auto generation = entry->state.generation;
    if (++entry->state.local_arrivals == participants) {
        entry->reported = true;
        if (local_host_ == 0) ArriveLocked(*entry, 0);
        else send_(MessageKind::kBarrierArrive, 0, id, generation, false);
    }
    entry->ready.wait(lock, [&] { return entry->broken || entry->state.generation != generation; });
    return entry->broken ? Status::FailedPrecondition("barrier is broken") : Status::Ok();
}

Status BarrierManager::Handle(MessageKind kind, HostId source, std::uint64_t id,
                              std::uint64_t generation, bool failed) {
    if (source >= host_count_) return Status::InvalidArgument("barrier source out of range");
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = Get(id); // Remote arrival may precede the first local API call.
    if (failed) {
        if (kind != MessageKind::kBarrierRelease) return Status::InvalidArgument("invalid barrier failure");
        // Failure is sticky; no retransmission or crash recovery is attempted.
        entry->broken = true;
        entry->ready.notify_all();
        return Status::Ok();
    }
    if (entry->broken || generation < entry->state.generation) return Status::Ok();
    if (generation != entry->state.generation) {
        BreakLocked(*entry);
        return Status::Ok();
    }
    if (kind == MessageKind::kBarrierArrive && local_host_ == 0 && source != 0) {
        ArriveLocked(*entry, source); // A repeated arrival contributes only one host bit.
        return Status::Ok();
    }
    if (kind == MessageKind::kBarrierRelease && local_host_ != 0 && source == 0 && entry->reported) {
        AdvanceLocked(*entry);
        return Status::Ok();
    }
    BreakLocked(*entry);
    return Status::Ok();
}

void BarrierManager::Break(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    BreakLocked(*Get(id));
}

bool BarrierManager::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : barriers_) {
        const auto& entry = *item.second;
        if (!entry.broken && (entry.host_arrivals || entry.state.local_arrivals)) return false;
    }
    return true;
}

Result<BarrierState> BarrierManager::Query(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = barriers_.find(id);
    if (it == barriers_.end()) return Status::NotFound("unknown barrier ID");
    return it->second->state;
}
}  // namespace cxloom::loompar
