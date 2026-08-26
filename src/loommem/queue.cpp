#include "cxloom/loommem/queue.h"

namespace cxloom::loommem {

SpscQueue::SpscQueue(std::size_t capacity_entries) : capacity_entries_(capacity_entries) {}

Status SpscQueue::Push(QueueEnvelope message) {
    if (entries_.size() >= capacity_entries_) {
        return Status::Unavailable("queue is full");
    }
    entries_.push_back(std::move(message));
    return Status::Ok();
}

Result<QueueEnvelope> SpscQueue::Pop() {
    if (entries_.empty()) {
        return Status::Unavailable("queue is empty");
    }
    QueueEnvelope value = std::move(entries_.front());
    entries_.pop_front();
    return value;
}

std::size_t SpscQueue::Size() const {
    return entries_.size();
}

}  // namespace cxloom::loommem

