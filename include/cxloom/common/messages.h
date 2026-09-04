#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cxloom/common/types.h"

namespace cxloom {

enum class MessageKind : std::uint16_t {
    kInvalid = 0,
    kCreateReq,
    kCreateAck,
    kCompleteNotify,
    kBarrierArrive,
    kBarrierRelease,
    kTokenReq,
    kTokenGrant,
    kLoadUpdate,
};

struct MessageHeader {
    MessageKind kind {MessageKind::kInvalid};
    HostId src_host {0};
    HostId dst_host {0};
    std::uint32_t payload_bytes {0};
};

struct CreateRequest {
    MessageHeader header {};
    GlobalThreadId gtid {};
    std::uint64_t function_id {0};
    std::vector<std::byte> arg_bytes;
};

struct CreateAck {
    MessageHeader header {};
    GlobalThreadId gtid {};
    std::uint32_t remote_running_threads {0};
};

struct CompleteNotify {
    MessageHeader header {};
    GlobalThreadId gtid {};
    std::int32_t exit_code {0};
    std::uint32_t remote_running_threads {0};
};

struct BarrierArrive {
    MessageHeader header {};
    std::uint64_t barrier_id {0};
    std::uint64_t generation {0};
};

struct BarrierRelease {
    MessageHeader header {};
    std::uint64_t barrier_id {0};
    std::uint64_t generation {0};
};

struct TokenRequest {
    MessageHeader header {};
    GlobalPointer object {};
    std::uint64_t generation {0};
    std::uint64_t request_id {0};
    Version observed_version {0};
    HostId requester {0};
    bool activate_coherence_epoch {true};
};

struct TokenGrant {
    MessageHeader header {};
    GlobalPointer object {};
    std::uint64_t generation {0};
    std::uint64_t request_id {0};
    HostId new_owner {0};
    Version version {0};
    std::uint64_t token_epoch {0};
    bool activate_coherence_epoch {true};
};

}  // namespace cxloom
