#pragma once

#include <cstdint>

#include "cxloom/common/types.h"

namespace cxloom::loompar {

// Fixed-layout metadata suitable for publication in a LoomMem allocation.
// Payloads and results are referenced by GPtr; no process-local addresses are
// embedded in this structure.
struct InvocationContext {
    GlobalThreadId gtid{};
    std::uint64_t function_id{0};
    std::uint32_t abi_version{1};
    std::uint32_t flags{0};
    GlobalPointer argument_gptr{};
    std::uint64_t argument_bytes{0};
    GlobalPointer result_gptr{};
    std::uint64_t result_bytes{0};
    std::uint64_t migration_epoch{0};
};

}  // namespace cxloom::loompar
