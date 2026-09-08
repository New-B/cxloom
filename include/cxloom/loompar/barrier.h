#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "cxloom/common/messages.h"
#include "cxloom/common/status.h"

namespace cxloom::loompar {

struct BarrierState {
    std::uint64_t barrier_id {0};
    std::uint64_t generation {0};
    std::size_t local_participants {0};
    std::size_t local_arrivals {0};
};

// Fixed world membership, coordinator host 0. The sender must enqueue without waiting.
class BarrierManager {
public:
    using Sender = std::function<void(MessageKind, HostId, std::uint64_t, std::uint64_t, bool)>;
    BarrierManager(HostId local_host = 0, std::uint16_t host_count = 1, Sender sender = {});
    Status Wait(std::uint64_t id, std::size_t local_participants);
    Status Handle(MessageKind kind, HostId source, std::uint64_t id,
                  std::uint64_t generation, bool failed);
    void Break(std::uint64_t id);
    bool idle() const;
    Result<BarrierState> Query(std::uint64_t id) const;

private:
    struct Entry {
        BarrierState state;
        std::uint64_t host_arrivals {0};
        bool reported {false};
        bool broken {false};
        std::condition_variable ready;
    };
    std::shared_ptr<Entry> Get(std::uint64_t id);
    void BreakLocked(Entry& entry);
    void ArriveLocked(Entry& entry, HostId source);
    void AdvanceLocked(Entry& entry);
    HostId local_host_;
    std::uint16_t host_count_;
    Sender send_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Entry>> barriers_;
};

}  // namespace cxloom::loompar
