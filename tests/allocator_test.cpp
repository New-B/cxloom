#include <cstddef>
#include <iostream>

#include "cxloom/loommem/allocator.h"

// This isolated allocator test has no caches or protocol activity. Explicitly
// certify the single participant before exercising the final reclaim primitive.
static bool Reclaim(cxloom::loommem::SharedExtentAllocator& allocator, cxloom::GlobalPointer object) {
    if (!allocator.BeginRetire(object, 1).ok()) return false;
    for (int phase = 0; phase < 3; ++phase)
        if (!allocator.AcknowledgeRetirement(allocator.Retirement(), 0, {}).ok()) return false;
    return allocator.Free(object).ok();
}

int main() {
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
    // Descriptor layouts are incompatible across allocator format revisions.
    header->layout_version = cxloom::loommem::kAllocatorLayoutVersion - 1;
    if (shared_allocator.Initialize().ok())
        return 1;
    header->layout_version = cxloom::loommem::kAllocatorLayoutVersion;
    if (!shared_allocator.Initialize().ok())
        return 1;

    // The default granule partitions large objects without a separate mode.
    const auto default_range = shared_allocator.Allocate(4097, 64);
    const auto default_info = default_range.ok() ? shared_allocator.Describe(default_range.value())
                                                : cxloom::Result<cxloom::loommem::AllocationInfo>(default_range.status());
    if (!default_info.ok() || default_info.value().coherence_block_bytes != 4096 ||
        default_info.value().coherence_block_count != 2 ||
        !shared_allocator.MutableCoherenceBlock(default_range.value(), 1).ok() ||
        !Reclaim(shared_allocator, default_range.value()))
        return 1;

    const auto object = shared_allocator.Allocate(256, 64);
    const auto object_info = object.ok() ? shared_allocator.Describe(object.value())
                                         : cxloom::Result<cxloom::loommem::AllocationInfo>(object.status());
    if (!object_info.ok() || object_info.value().coherence_block_count != 1 ||
        object_info.value().coherence_block_bytes != 4096 ||
        !shared_allocator.MutableCoherenceBlock(object.value(), 0).ok()) {
        std::cerr << "single-block allocation metadata is invalid\n";
        return 1;
    }

    cxloom::loommem::AllocationOptions block_options;
    block_options.bytes = 1000;
    block_options.alignment = 64;
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
    if (!Reclaim(shared_allocator, object.value()) || !Reclaim(shared_allocator, blocked.value()))
        return 1;
    cxloom::loommem::AllocationOptions merged_options;
    merged_options.bytes = 1200;
    merged_options.alignment = 64;
    merged_options.coherence_block_bytes = 128;
    const auto merged = shared_allocator.Allocate(merged_options);
    const auto merged_info = merged.ok() ? shared_allocator.Describe(merged.value())
                                          : cxloom::Result<cxloom::loommem::AllocationInfo>(merged.status());
    if (!merged_info.ok() || merged.value().offset != first_offset ||
        merged_info.value().coherence_block_count != 10) {
        std::cerr << "coalesced extents did not support a fresh differently shaped allocation\n";
        return 1;
    }
    return 0;
}
