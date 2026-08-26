#include "cxloom/common/tracing.h"

#include <iostream>

namespace cxloom {

void Trace(const std::string& component, const std::string& message) {
    std::cerr << "[" << component << "] " << message << "\n";
}

}  // namespace cxloom

