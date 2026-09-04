#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"
#include "cxloom/loommem/allocator.h"
#include "cxloom/loommem/coherence.h"
#include "cxloom/loommem/layout.h"
#include "cxloom/loommem/poller.h"
#include "cxloom/loommem/queue.h"
#include "cxloom/loommem/region_mapper.h"
#include "cxloom/loommem/token.h"
#include "cxloom/loommem/visibility.h"

namespace cxloom::loommem {

enum class ReadConsistency { kPerBlock, kWholeRange };
enum class WriteAtomicity { kPerBlock, kWholeRange };

struct PublishedSharedObject {
    GlobalPointer gptr {};
    std::uint64_t bytes {0};
};

struct ReadSnapshot {
    GlobalPointer object {};
    std::uint64_t offset {0};
    std::uint64_t allocation_id {0};
    Version version {0};
    std::vector<Version> block_versions;
    std::shared_ptr<const std::vector<std::byte>> storage;

    const void* data() const { return storage == nullptr ? nullptr : storage->data(); }
    std::size_t bytes() const { return storage == nullptr ? 0 : storage->size(); }
};

struct WriteBuffer {
    TokenLease lease {};
    std::vector<TokenLease> leases;
    std::uint64_t offset {0};
    std::shared_ptr<std::vector<std::byte>> storage;
    mutable std::shared_ptr<void> reference_guard;
    WriteAtomicity atomicity {WriteAtomicity::kPerBlock};

    void* data() { return storage == nullptr ? nullptr : storage->data(); }
    const void* data() const { return storage == nullptr ? nullptr : storage->data(); }
    std::size_t bytes() const { return storage == nullptr ? 0 : storage->size(); }
};

struct ObjectReference {
    GlobalPointer object {};
    std::uint64_t allocation_id {0};
    std::shared_ptr<void> guard;
};

class LoomMemRuntime {
  public:
    explicit LoomMemRuntime(CxloomConfig config);

    Status Initialize();
    Status Finalize();

    const SharedRegionLayout& layout() const { return layout_; }
    GlobalAllocator& allocator() { return *allocator_; }
    CoherenceManager& coherence() { return *coherence_; }

    Result<GlobalPointer> AllocateShared(std::size_t bytes, std::size_t alignment);
    Result<GlobalPointer> AllocateShared(const AllocationOptions& options);
    Status FreeShared(GlobalPointer gptr);
    Result<ObjectReference> AcquireObjectReference(GlobalPointer gptr);
    Result<void*> ResolveLocal(const GlobalPointer& gptr) const;
    Result<AllocationInfo> DescribeSharedAllocation(GlobalPointer gptr) const;
    Result<HostId> ResolveOwningHost(GlobalPointer gptr) const;
    Status PublishBootstrapProbe(std::uint64_t value);
    Status WaitForAllHosts(std::uint64_t timeout_ms) const;
    Result<std::uint64_t> ReadBootstrapProbe(HostId host) const;
    std::uint32_t joined_host_count() const;
    Status PublishSharedObject(GlobalPointer gptr, std::uint64_t bytes,
                               VisibilityMode mode = VisibilityMode::kReleaseAcquire);
    Status WaitForAllSharedObjects(std::uint64_t timeout_ms,
                                   VisibilityMode mode = VisibilityMode::kReleaseAcquire) const;
    Result<PublishedSharedObject> ReadPublishedSharedObject(HostId host) const;
    Status PublishVisibilitySequence(std::uint64_t sequence, VisibilityMode mode);
    Status WaitForVisibilitySequence(std::uint64_t sequence, std::uint64_t timeout_ms, VisibilityMode mode) const;
    Status PublishObservedSequence(std::uint64_t sequence, std::uint64_t errors, VisibilityMode mode);
    Status WaitForObservedSequence(std::uint64_t sequence, std::uint64_t timeout_ms, VisibilityMode mode) const;
    std::uint64_t visibility_error_count() const;
    Result<HostId> ResolvePreferredHost(const GlobalPointer& gptr) const;
    Result<SpscQueue*> GetQueue(HostId producer, HostId consumer);
    Status StartQueuePoller(QueueMessageHandler handler = {}, QueuePollerOptions options = {});
    Status StopQueuePoller();
    Result<TokenRequestHandle> RequestWriteToken(GlobalPointer object, std::uint64_t block_index = 0);
    Result<TokenLease> WaitForWriteToken(const TokenRequestHandle& request, std::uint64_t timeout_ms);
    Status CancelWriteTokenRequest(const TokenRequestHandle& request);
    Status CancelWriteTokenRequestAndWait(const TokenRequestHandle& request, std::uint64_t timeout_ms);
    Status ReleaseWriteToken(const TokenLease& lease);
    Result<ReadSnapshot> AcquireReadSnapshot(GlobalPointer object, std::uint64_t timeout_ms);
    Result<WriteBuffer> AcquireWriteBuffer(GlobalPointer object, std::uint64_t timeout_ms);
    Result<ReadSnapshot> AcquireReadRange(GlobalPointer object, std::uint64_t offset,
                                          std::uint64_t bytes, std::uint64_t timeout_ms,
                                          ReadConsistency consistency = ReadConsistency::kPerBlock);
    Result<WriteBuffer> AcquireWriteRange(GlobalPointer object, std::uint64_t offset,
                                          std::uint64_t bytes, std::uint64_t timeout_ms,
                                          WriteAtomicity atomicity = WriteAtomicity::kPerBlock);
    Status ReleaseWriteBuffer(const WriteBuffer& write);
    Status AbortWriteBuffer(const WriteBuffer& write);
    const QueuePoller* queue_poller() const { return queue_poller_.get(); }
    std::size_t queue_capacity_entries() const { return config_.queue_capacity_entries; }
    std::size_t cached_replica_count() const;
    std::size_t cached_replica_bytes() const;
    std::size_t pending_token_request_count() const;
    const RegionMapper& region_mapper() const { return region_mapper_; }

  private:
    CxloomConfig config_;
    SharedRegionLayout layout_ {};
    RegionMapper region_mapper_;
    BootstrapHeader* bootstrap_ {nullptr};
    AllocatorHeader* allocator_header_ {nullptr};
    CoherenceRegionHeader* coherence_header_ {nullptr};
    std::unique_ptr<GlobalAllocator> allocator_;
    std::unique_ptr<CoherenceManager> coherence_;
    std::vector<std::vector<std::unique_ptr<SpscQueue>>> queues_;
    std::unique_ptr<QueuePoller> queue_poller_;
    std::unique_ptr<TokenService> token_service_;
    struct CachedReplica {
        std::uint64_t object_offset {0};
        std::uint64_t block_index {0};
        std::uint64_t allocation_id {0};
        Version version {0};
        std::shared_ptr<const std::vector<std::byte>> storage;
        std::uint64_t last_access {0};
    };
    mutable std::mutex replicas_mutex_;
    std::unordered_map<std::uint64_t, CachedReplica> replicas_;
    std::size_t cached_replica_bytes_ {0};
    std::uint64_t replica_access_clock_ {0};
    bool initialized_ {false};

    Status InitializeBootstrap();
    Status AttachBootstrap();
    Status ValidateBootstrap(const BootstrapHeader& header) const;
    Status RegisterLocalHost();
    void CacheReplica(std::uint64_t cache_key, std::uint64_t object_offset, std::uint64_t block_index,
                      std::uint64_t allocation_id, Version version,
                      std::shared_ptr<const std::vector<std::byte>> storage);
    void EvictReplicasLocked();
};

}  // namespace cxloom::loommem
