#pragma once

#include <cstddef>
#include <string>

#include "cxloom/common/status.h"

namespace cxloom::loommem {

enum class VisibilityMode {
    kReleaseAcquire = 0,
    kSequentiallyConsistent,
    kClflush,
    kClwb,
};

Result<VisibilityMode> ParseVisibilityMode(const std::string& name);
const char* VisibilityModeName(VisibilityMode mode);
Status ValidateVisibilityMode(VisibilityMode mode);
Status PublishData(const void* address, std::size_t bytes, VisibilityMode mode);
Status AcquireData(const void* address, std::size_t bytes, VisibilityMode mode);

}  // namespace cxloom::loommem
