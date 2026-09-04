#include <cstddef>
#include <iostream>

#include "cxloom/loommem/allocator.h"

int main() {
    cxloom::loommem::SlabExtentAllocator allocator(2ULL << 20);
    if (!allocator.Initialize().ok())
        return 1;

    const auto small_a = allocator.Allocate(24, 16);
    const auto small_b = allocator.Allocate(24, 16);
    const auto large = allocator.Allocate(128ULL << 10, 4096);
    if (!small_a.ok() || !small_b.ok() || !large.ok())
        return 1;
    if (!allocator.Free(small_a.value()).ok() || !allocator.Free(large.value()).ok())
        return 1;

    const auto reused_small = allocator.Allocate(24, 16);
    const auto reused_large = allocator.Allocate(128ULL << 10, 4096);
    if (!reused_small.ok() || !reused_large.ok() || reused_small.value().offset != small_a.value().offset ||
        reused_large.value().offset != large.value().offset) {
        std::cerr << "allocator did not reuse freed memory\n";
        return 1;
    }
    if (!allocator.Free(small_b.value()).ok())
        return 1;

    alignas(64) std::byte shared_region[8192] {};
    auto* header = reinterpret_cast<cxloom::loommem::AllocatorHeader*>(shared_region);
    if (!cxloom::loommem::FormatSharedAllocator(header, 4096, 4096, 4096).ok()) {
        return 1;
    }

    cxloom::loommem::SharedBumpAllocator shared_allocator(header, shared_region, sizeof(shared_region), 0, 4096, 4096);
    if (!shared_allocator.Initialize().ok())
        return 1;

    const auto whole_extent = shared_allocator.Allocate(3968, 64);
    if (!whole_extent.ok() || whole_extent.value().offset != 4224 || shared_allocator.Allocate(1, 1).ok()) {
        std::cerr << "shared allocator did not enforce its extent boundary\n";
        return 1;
    }
    return 0;
}
