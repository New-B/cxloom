#pragma once

#include <memory>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"
#include "cxloom/loommem/allocator.h"
#include "cxloom/loommem/coherence.h"
#include "cxloom/loommem/layout.h"
#include "cxloom/loommem/queue.h"

namespace cxloom::loommem {

class LoomMemRuntime {
public:
    explicit LoomMemRuntime(CxloomConfig config);

    Status Initialize();
    Status Finalize();

    const SharedRegionLayout& layout() const { return layout_; }
    GlobalAllocator& allocator() { return *allocator_; }
    CoherenceManager& coherence() { return *coherence_; }

    Result<GlobalPointer> AllocateShared(std::size_t bytes, std::size_t alignment);
    Status FreeShared(GlobalPointer gptr);
    Result<HostId> ResolvePreferredHost(const GlobalPointer& gptr) const;
    Result<SpscQueue*> GetQueue(HostId producer, HostId consumer);

private:
    CxloomConfig config_;
    SharedRegionLayout layout_ {};
    std::unique_ptr<GlobalAllocator> allocator_;
    std::unique_ptr<CoherenceManager> coherence_;
    std::vector<std::vector<std::unique_ptr<SpscQueue>>> queues_;
    bool initialized_ {false};
};

}  // namespace cxloom::loommem
