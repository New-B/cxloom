#pragma once

#include <deque>
#include <vector>

#include "cxloom/common/messages.h"
#include "cxloom/common/status.h"

namespace cxloom::loommem {

struct QueueEnvelope {
    MessageHeader header {};
    std::vector<std::byte> payload;
};

class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity_entries);

    Status Push(QueueEnvelope message);
    Result<QueueEnvelope> Pop();
    std::size_t Size() const;

private:
    std::size_t capacity_entries_ {0};
    std::deque<QueueEnvelope> entries_;
};

}  // namespace cxloom::loommem

