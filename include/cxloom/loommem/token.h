#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "cxloom/common/messages.h"
#include "cxloom/common/status.h"
#include "cxloom/loommem/allocator.h"
#include "cxloom/loommem/queue.h"

namespace cxloom::loommem {

struct TokenRequestHandle {
    GlobalPointer object {};
    std::uint64_t block_index {0};
    std::uint64_t allocation_id {0};
    std::uint64_t request_id {0};
};

struct TokenLease {
    GlobalPointer object {};
    std::uint64_t block_index {0};
    std::uint64_t allocation_id {0};
    Version version {0};
    std::uint64_t token_epoch {0};
};

using TokenQueueResolver = std::function<Result<SpscQueue*>(HostId, HostId)>;

class TokenService {
  public:
    TokenService(HostId local_host, SharedExtentAllocator* allocator, void* region_base,
                 TokenQueueResolver queue_resolver,
                 VisibilityMode mode = VisibilityMode::kReleaseAcquire);

    Result<TokenRequestHandle> Request(GlobalPointer object, std::uint64_t block_index = 0,
                                       bool activate_coherence_epoch = true);
    Status RegisterAllocation(GlobalPointer object);
    Result<TokenLease> Wait(const TokenRequestHandle& request, std::uint64_t timeout_ms);
    Status Cancel(const TokenRequestHandle& request);
    Status CancelAndWait(const TokenRequestHandle& request, std::uint64_t timeout_ms);
    Status BeginWriteback(const TokenLease& lease);
    Status Release(const TokenLease& lease, bool modified = true);
    Status HandleMessage(const QueueEnvelope& envelope);
    Status PrepareRetire(GlobalPointer object, std::uint64_t timeout_ms);
    void ForgetAllocation(GlobalPointer object, std::uint64_t coherence_metadata_offset,
                          std::uint64_t block_count);
    std::size_t pending_request_count() const;

  private:
    struct CoherenceTarget {
        AllocationDescriptor* allocation {nullptr};
        CoherenceBlockDescriptor* block {nullptr};
        std::uint64_t local_key {0};
    };
    struct Waiter {
        enum class State { kActive, kCancelling, kGranted, kRejected, kCancelled };
        State state {State::kActive};
        bool granted {false};
        bool abandoned {false};
        bool retain_completion {false};
        TokenLease lease {};
        Status completion_status {};
        std::condition_variable ready;
    };
    struct LocalObject {
        bool available {false};
        bool held {false};
        std::deque<TokenRequest> pending;
    };
    struct RetirementWaiter {
        GlobalPointer object {};
        std::uint64_t allocation_id {0};
        std::uint64_t remaining {0};
        std::unordered_set<std::uint64_t> acknowledged_blocks;
        std::condition_variable ready;
    };

    Result<CoherenceTarget> Target(GlobalPointer object, std::uint64_t block_index,
                                   bool allow_retiring = false) const;
    Status SendRequest(HostId destination, const TokenRequest& request);
    Status Grant(const TokenRequest& request, const CoherenceTarget& target);
    Status HandleRequest(const TokenRequest& request);
    Status HandleGrant(const TokenGrant& grant);
    Status HandleReject(const TokenReject& reject);
    Status HandleCancel(const TokenCancel& cancel);
    Status HandleCancelAck(const TokenCancelAck& ack);
    Status HandleRetire(const TokenRetire& retire);
    Status HandleRetireAck(const TokenRetireAck& ack);
    Status Reject(const TokenRequest& request, TokenCompletionReason reason);
    Status AcknowledgeCancel(const TokenCancel& cancel, TokenCompletionReason reason);
    Status SendCancel(const TokenCancel& cancel);
    Status ActivateWriter(CoherenceBlockDescriptor* block);
    Status PushWithBackpressure(HostId destination, QueueEnvelope envelope);

    HostId local_host_ {0};
    SharedExtentAllocator* allocator_ {nullptr};
    std::byte* region_base_ {nullptr};
    TokenQueueResolver queue_resolver_;
    VisibilityMode mode_ {VisibilityMode::kReleaseAcquire};
    std::atomic<std::uint64_t> next_request_id_ {1};
    std::array<std::mutex, kMaxHosts> outbound_mutexes_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, LocalObject> objects_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Waiter>> waiters_;
    std::unordered_map<std::uint64_t, std::shared_ptr<RetirementWaiter>> retirement_waiters_;
};

}  // namespace cxloom::loommem
