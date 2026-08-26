#pragma once

#include "cxloom/common/status.h"
#include "cxloom/loommem/layout.h"

namespace cxloom::loommem {

class CoherenceManager {
public:
    virtual ~CoherenceManager() = default;

    virtual Status AcquireRead(ObjectMetadata& object, ReplicaMetadata& replica) = 0;
    virtual Status AcquireWrite(ObjectMetadata& object, ReplicaMetadata& replica) = 0;
    virtual Status ReleaseWrite(ObjectMetadata& object, ReplicaMetadata& replica) = 0;
};

class TokenCoherenceManager final : public CoherenceManager {
public:
    explicit TokenCoherenceManager(HostId local_host);

    Status AcquireRead(ObjectMetadata& object, ReplicaMetadata& replica) override;
    Status AcquireWrite(ObjectMetadata& object, ReplicaMetadata& replica) override;
    Status ReleaseWrite(ObjectMetadata& object, ReplicaMetadata& replica) override;

private:
    HostId local_host_ {0};
};

}  // namespace cxloom::loommem

