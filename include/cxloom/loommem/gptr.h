#pragma once

#include <cstddef>
#include <cstdint>

#include "cxloom/common/types.h"

namespace cxloom::loommem {

inline std::uintptr_t ResolveLocalAddress(std::uintptr_t base, const GlobalPointer& gptr) {
    return base + static_cast<std::uintptr_t>(gptr.offset);
}

inline GlobalPointer MakeGlobalPointer(std::uint32_t region_id, std::uint64_t offset) {
    return GlobalPointer{region_id, offset};
}

}  // namespace cxloom::loommem

