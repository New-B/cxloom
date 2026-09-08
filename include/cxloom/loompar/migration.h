#pragma once

#include <cstdint>
#include <limits>
#include "cxloom/common/status.h"
#include "cxloom/common/types.h"

namespace cxloom::loompar {

// Home-owned protocol model. Callers serialize access and authenticate message
// endpoints. This does not move a native stack or dispatch transport messages.
class MigrationTransaction {
public:
    enum class Phase { kIdle, kRequested, kQuiesced, kPrepared, kCommitted, kAborted };
    explicit MigrationTransaction(HostId owner, std::uint16_t hosts)
        : owner_(owner), hosts_(hosts) {}

    Result<std::uint64_t> Begin(HostId target) {
        if (owner_ >= hosts_ || target >= hosts_ || target == owner_)
            return Status::InvalidArgument("migration requires distinct valid hosts");
        if (phase_ != Phase::kIdle && phase_ != Phase::kAborted)
            return Status::FailedPrecondition("migration already active");
        if (epoch_ == std::numeric_limits<std::uint64_t>::max())
            return Status::Unavailable("migration epoch exhausted");
        source_ = owner_;
        target_ = target;
        phase_ = Phase::kRequested;
        return ++epoch_;
    }
    Status Quiesced(HostId source, std::uint64_t epoch) {
        if (epoch != epoch_ || source != source_)
            return Status::FailedPrecondition("stale or foreign quiescence");
        if (phase_ == Phase::kQuiesced || phase_ == Phase::kPrepared) return Status::Ok();
        if (phase_ != Phase::kRequested) return Status::FailedPrecondition("unexpected quiescence");
        phase_ = Phase::kQuiesced;
        return Status::Ok();
    }
    Status Prepared(HostId target, std::uint64_t epoch) {
        if (epoch != epoch_ || target != target_)
            return Status::FailedPrecondition("stale or foreign preparation");
        if (phase_ == Phase::kPrepared) return Status::Ok();
        if (phase_ != Phase::kQuiesced) return Status::FailedPrecondition("source is not quiescent");
        phase_ = Phase::kPrepared;
        return Status::Ok();
    }
    Status Commit(std::uint64_t epoch) {
        if (epoch != epoch_) return Status::FailedPrecondition("stale commit");
        if (phase_ == Phase::kCommitted) return Status::Ok();
        if (phase_ != Phase::kPrepared) return Status::FailedPrecondition("destination is not prepared");
        owner_ = target_;
        execution_epoch_ = epoch_;
        phase_ = Phase::kCommitted;
        return Status::Ok();
    }
    Status Resumed(HostId target, std::uint64_t epoch) {
        if (epoch != epoch_ || target != owner_ || phase_ != Phase::kCommitted)
            return Status::FailedPrecondition("unexpected resume acknowledgement");
        phase_ = Phase::kIdle;
        return Status::Ok();
    }
    Status Abort(std::uint64_t epoch) {
        if (epoch != epoch_ || phase_ == Phase::kCommitted || phase_ == Phase::kIdle)
            return Status::FailedPrecondition("cannot abort this transaction");
        phase_ = Phase::kAborted;
        return Status::Ok();
    }
    bool AcceptsCompletion(HostId host, std::uint64_t execution_epoch) const {
        return host == owner_ && execution_epoch == execution_epoch_ &&
            (phase_ == Phase::kIdle || phase_ == Phase::kRequested ||
             phase_ == Phase::kAborted || phase_ == Phase::kCommitted);
    }
    HostId owner() const { return owner_; }
    Phase phase() const { return phase_; }
private:
    HostId owner_, source_{0}, target_{0};
    std::uint16_t hosts_;
    std::uint64_t epoch_{0}, execution_epoch_{0};
    Phase phase_{Phase::kIdle};
};
}  // namespace cxloom::loompar
