#include "cxloom/loompar/barrier.h"

namespace cxloom::loompar {

Status BarrierManager::RegisterBarrier(std::uint64_t barrier_id, std::size_t local_participants) {
    auto it = barriers_.find(barrier_id);
    if (it != barriers_.end()) {
        return Status::AlreadyExists("barrier is already registered");
    }
    barriers_.emplace(barrier_id, BarrierState{barrier_id, 0, local_participants, 0});
    return Status::Ok();
}

Status BarrierManager::Arrive(std::uint64_t barrier_id) {
    auto it = barriers_.find(barrier_id);
    if (it == barriers_.end()) {
        return Status::NotFound("unknown barrier id");
    }
    it->second.local_arrivals += 1;
    if (it->second.local_arrivals >= it->second.local_participants) {
        it->second.generation += 1;
        it->second.local_arrivals = 0;
    }
    return Status::Ok();
}

Result<BarrierState> BarrierManager::Query(std::uint64_t barrier_id) const {
    auto it = barriers_.find(barrier_id);
    if (it == barriers_.end()) {
        return Status::NotFound("unknown barrier id");
    }
    return it->second;
}

}  // namespace cxloom::loompar

