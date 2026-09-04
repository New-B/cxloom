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

    alignas(64) static std::byte shared_region[1ULL << 20] {};
    auto* header = reinterpret_cast<cxloom::loommem::AllocatorHeader*>(shared_region);
    auto* coherence_header =
        reinterpret_cast<cxloom::loommem::CoherenceRegionHeader*>(shared_region + (256ULL << 10));
    if (!cxloom::loommem::FormatSharedAllocator(header, 256ULL << 10, 512ULL << 10, 512ULL << 10,
                                                256ULL << 10, 256ULL << 10).ok() ||
        !cxloom::loommem::FormatCoherenceRegion(coherence_header, 256ULL << 10, 256ULL << 10).ok()) {
        return 1;
    }

    cxloom::loommem::SharedExtentAllocator shared_allocator(header, shared_region, sizeof(shared_region), 0,
                                                          512ULL << 10, 512ULL << 10, coherence_header);
    if (!shared_allocator.Initialize().ok())
        return 1;

    const auto object = shared_allocator.Allocate(256, 64);
    const auto object_info = object.ok() ? shared_allocator.Describe(object.value())
                                         : cxloom::Result<cxloom::loommem::AllocationInfo>(object.status());
    if (!object_info.ok() || object_info.value().coherence_block_count != 1 ||
        object_info.value().coherence_block_bytes != 256 ||
        !shared_allocator.MutableCoherenceBlock(object.value(), 0).ok()) {
        std::cerr << "object-granularity allocation metadata is invalid\n";
        return 1;
    }

    cxloom::loommem::AllocationOptions block_options;
    block_options.bytes = 1000;
    block_options.alignment = 64;
    block_options.coherence_granularity = cxloom::CoherenceGranularity::kFixedBlock;
    block_options.coherence_block_bytes = 256;
    const auto blocked = shared_allocator.Allocate(block_options);
    const auto blocked_info = blocked.ok() ? shared_allocator.Describe(blocked.value())
                                            : cxloom::Result<cxloom::loommem::AllocationInfo>(blocked.status());
    if (!blocked_info.ok() || blocked_info.value().coherence_block_count != 4 ||
        blocked_info.value().coherence_block_bytes != 256 ||
        !shared_allocator.MutableCoherenceBlock(blocked.value(), 3).ok() ||
        shared_allocator.MutableCoherenceBlock(blocked.value(), 4).ok()) {
        std::cerr << "fixed-block allocation metadata is invalid\n";
        return 1;
    }
    const auto first_offset = object.value().offset;
    const auto first_id = object_info.value().allocation_id;
    if (!shared_allocator.Free(object.value()).ok() || !shared_allocator.Free(blocked.value()).ok())
        return 1;
    cxloom::loommem::AllocationOptions merged_options;
    merged_options.bytes = 1200;
    merged_options.alignment = 64;
    merged_options.coherence_granularity = cxloom::CoherenceGranularity::kFixedBlock;
    merged_options.coherence_block_bytes = 128;
    const auto merged = shared_allocator.Allocate(merged_options);
    const auto merged_info = merged.ok() ? shared_allocator.Describe(merged.value())
                                          : cxloom::Result<cxloom::loommem::AllocationInfo>(merged.status());
    if (!merged_info.ok() || merged.value().offset != first_offset ||
        merged_info.value().allocation_id <= first_id || merged_info.value().coherence_block_count != 10) {
        std::cerr << "coalesced extents did not support a fresh differently shaped allocation\n";
        return 1;
    }
    return 0;
}
