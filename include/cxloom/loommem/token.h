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

#include "cxloom/common/messages.h"
#include "cxloom/common/status.h"
#include "cxloom/loommem/allocator.h"
#include "cxloom/loommem/queue.h"

namespace cxloom::loommem {

struct TokenRequestHandle {
    GlobalPointer object {};
    std::uint64_t generation {0};
    std::uint64_t request_id {0};
};

struct TokenLease {
    GlobalPointer object {};
    std::uint64_t generation {0};
    Version version {0};
    std::uint64_t token_epoch {0};
};

using TokenQueueResolver = std::function<Result<SpscQueue*>(HostId, HostId)>;

class TokenService {
  public:
    TokenService(HostId local_host, SharedBumpAllocator* allocator, void* region_base,
                 TokenQueueResolver queue_resolver,
                 VisibilityMode mode = VisibilityMode::kReleaseAcquire);

    Result<TokenRequestHandle> Request(GlobalPointer object);
    Status RegisterAllocation(GlobalPointer object);
    Result<TokenLease> Wait(const TokenRequestHandle& request, std::uint64_t timeout_ms);
    Status Release(const TokenLease& lease);
    Status HandleMessage(const QueueEnvelope& envelope);

  private:
    struct Waiter {
        bool granted {false};
        TokenLease lease {};
        std::condition_variable ready;
    };
    struct LocalObject {
        bool available {false};
        bool held {false};
        std::deque<TokenRequest> pending;
    };

    Result<AllocationDescriptor*> Descriptor(GlobalPointer object) const;
    Status SendRequest(HostId destination, const TokenRequest& request);
    Status Grant(const TokenRequest& request, AllocationDescriptor* descriptor);
    Status HandleRequest(const TokenRequest& request);
    Status HandleGrant(const TokenGrant& grant);
    Status PushWithBackpressure(HostId destination, QueueEnvelope envelope);

    HostId local_host_ {0};
    SharedBumpAllocator* allocator_ {nullptr};
    std::byte* region_base_ {nullptr};
    TokenQueueResolver queue_resolver_;
    VisibilityMode mode_ {VisibilityMode::kReleaseAcquire};
    std::atomic<std::uint64_t> next_request_id_ {1};
    std::array<std::mutex, kMaxHosts> outbound_mutexes_;
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, LocalObject> objects_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Waiter>> waiters_;
};

}  // namespace cxloom::loommem
