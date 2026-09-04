#include "cxloom/loommem/visibility.h"

#include <atomic>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace cxloom::loommem {
namespace {

constexpr std::uintptr_t kCacheLineBytes = 64;

#if defined(__x86_64__) || defined(__i386__)
bool CpuHasClwb() {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7)
        return false;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
#ifdef bit_CLWB
    return (ebx & bit_CLWB) != 0;
#else
    return (ebx & (1U << 24)) != 0;
#endif
}

void ForEachCacheLine(const void* address, std::size_t bytes, void (*flush)(const void*)) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address) & ~(kCacheLineBytes - 1);
    const auto raw_end = reinterpret_cast<std::uintptr_t>(address) + bytes;
    for (auto line = begin; line < raw_end; line += kCacheLineBytes) {
        flush(reinterpret_cast<const void*>(line));
    }
}

void ClflushLine(const void* address) { _mm_clflush(address); }

void ClwbLine(const void* address) { asm volatile("clwb (%0)" : : "r"(address) : "memory"); }
#endif

}  // namespace

Result<VisibilityMode> ParseVisibilityMode(const std::string& name) {
    if (name == "release")
        return VisibilityMode::kReleaseAcquire;
    if (name == "seq_cst")
        return VisibilityMode::kSequentiallyConsistent;
    if (name == "clflush")
        return VisibilityMode::kClflush;
    if (name == "clwb")
        return VisibilityMode::kClwb;
    return Status::InvalidArgument("unknown visibility mode: " + name);
}

const char* VisibilityModeName(VisibilityMode mode) {
    switch (mode) {
    case VisibilityMode::kReleaseAcquire:
        return "release";
    case VisibilityMode::kSequentiallyConsistent:
        return "seq_cst";
    case VisibilityMode::kClflush:
        return "clflush";
    case VisibilityMode::kClwb:
        return "clwb";
    }
    return "unknown";
}

Status ValidateVisibilityMode(VisibilityMode mode) {
#if defined(__x86_64__) || defined(__i386__)
    if (mode == VisibilityMode::kClwb && !CpuHasClwb()) {
        return Status::Unavailable("CLWB is not supported by this CPU");
    }
    return Status::Ok();
#else
    if (mode == VisibilityMode::kClflush || mode == VisibilityMode::kClwb) {
        return Status::Unavailable("cache-line writeback mode is only implemented for x86");
    }
    return Status::Ok();
#endif
}

Status PublishData(const void* address, std::size_t bytes, VisibilityMode mode) {
    if (address == nullptr || bytes == 0) {
        return Status::InvalidArgument("visibility publication range must be non-empty");
    }
    const auto validation = ValidateVisibilityMode(mode);
    if (!validation.ok())
        return validation;

    switch (mode) {
    case VisibilityMode::kReleaseAcquire:
        std::atomic_thread_fence(std::memory_order_release);
        return Status::Ok();
    case VisibilityMode::kSequentiallyConsistent:
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return Status::Ok();
    case VisibilityMode::kClflush:
#if defined(__x86_64__) || defined(__i386__)
        ForEachCacheLine(address, bytes, ClflushLine);
        _mm_mfence();
#endif
        return Status::Ok();
    case VisibilityMode::kClwb:
#if defined(__x86_64__) || defined(__i386__)
        ForEachCacheLine(address, bytes, ClwbLine);
        _mm_sfence();
#endif
        return Status::Ok();
    }
    return Status::InvalidArgument("invalid visibility mode");
}

Status AcquireData(const void* address, std::size_t bytes, VisibilityMode mode) {
    if (address == nullptr || bytes == 0) {
        return Status::InvalidArgument("visibility acquisition range must be non-empty");
    }
    const auto validation = ValidateVisibilityMode(mode);
    if (!validation.ok())
        return validation;

    switch (mode) {
    case VisibilityMode::kReleaseAcquire:
        std::atomic_thread_fence(std::memory_order_acquire);
        return Status::Ok();
    case VisibilityMode::kSequentiallyConsistent:
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return Status::Ok();
    case VisibilityMode::kClflush:
    case VisibilityMode::kClwb:
#if defined(__x86_64__) || defined(__i386__)
        // CLWB does not invalidate a reader cache line. Both writeback
        // recipes therefore use CLFLUSH on the acquiring side.
        ForEachCacheLine(address, bytes, ClflushLine);
        _mm_mfence();
#endif
        std::atomic_thread_fence(std::memory_order_acquire);
        return Status::Ok();
    }
    return Status::InvalidArgument("invalid visibility mode");
}

}  // namespace cxloom::loommem
