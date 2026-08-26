#include <iostream>

#include "cxloom/loommem/allocator.h"

int main() {
    cxloom::loommem::SlabExtentAllocator allocator(2ULL << 20);
    if (!allocator.Initialize().ok()) {
        return 1;
    }

    const auto small_a = allocator.Allocate(24, 16);
    const auto small_b = allocator.Allocate(24, 16);
    const auto large = allocator.Allocate(128ULL << 10, 4096);
    if (!small_a.ok() || !small_b.ok() || !large.ok()) {
        return 1;
    }
    if (!allocator.Free(small_a.value()).ok() || !allocator.Free(large.value()).ok()) {
        return 1;
    }

    const auto reused_small = allocator.Allocate(24, 16);
    const auto reused_large = allocator.Allocate(128ULL << 10, 4096);
    if (!reused_small.ok() || !reused_large.ok() || reused_small.value().offset != small_a.value().offset ||
        reused_large.value().offset != large.value().offset) {
        std::cerr << "allocator did not reuse freed memory\n";
        return 1;
    }
    return allocator.Free(small_b.value()).ok() ? 0 : 1;
}
