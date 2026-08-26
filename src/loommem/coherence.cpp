#include "cxloom/loommem/coherence.h"

namespace cxloom::loommem {

TokenCoherenceManager::TokenCoherenceManager(HostId local_host) : local_host_(local_host) {}

Status TokenCoherenceManager::AcquireRead(ObjectMetadata& object, ReplicaMetadata& replica) {
    if (replica.cached && replica.local_version == object.global_version) {
        return Status::Ok();
    }
    replica.local_version = object.global_version;
    replica.cached = true;
    replica.dirty = false;
    return Status::Ok();
}

Status TokenCoherenceManager::AcquireWrite(ObjectMetadata& object, ReplicaMetadata& replica) {
    if (object.token_owner != local_host_) {
        return Status::Unimplemented("remote token transfer path is not implemented yet");
    }
    replica.cached = true;
    return Status::Ok();
}

Status TokenCoherenceManager::ReleaseWrite(ObjectMetadata& object, ReplicaMetadata& replica) {
    if (!replica.cached) {
        return Status::FailedPrecondition("write release requires a cached replica");
    }
    replica.dirty = false;
    object.global_version += 1;
    replica.local_version = object.global_version;
    return Status::Ok();
}

}  // namespace cxloom::loommem

