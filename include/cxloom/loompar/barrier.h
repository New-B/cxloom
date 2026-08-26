#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "cxloom/common/status.h"

namespace cxloom::loompar {

struct BarrierState {
    std::uint64_t barrier_id {0};
    std::uint64_t generation {0};
    std::size_t local_participants {0};
    std::size_t local_arrivals {0};
};

class BarrierManager {
public:
    Status RegisterBarrier(std::uint64_t barrier_id, std::size_t local_participants);
    Status Arrive(std::uint64_t barrier_id);
    Result<BarrierState> Query(std::uint64_t barrier_id) const;

private:
    std::unordered_map<std::uint64_t, BarrierState> barriers_;
};

}  // namespace cxloom::loompar

