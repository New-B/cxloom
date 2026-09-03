#pragma once

#include <memory>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"
#include "cxloom/loommem/allocator.h"
#include "cxloom/loommem/coherence.h"
#include "cxloom/loommem/layout.h"
#include "cxloom/loommem/queue.h"
#include "cxloom/loommem/region_mapper.h"

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
    Result<void*> ResolveLocal(const GlobalPointer& gptr) const;
    Status PublishBootstrapProbe(std::uint64_t value);
    Status WaitForAllHosts(std::uint64_t timeout_ms) const;
    Result<std::uint64_t> ReadBootstrapProbe(HostId host) const;
    std::uint32_t joined_host_count() const;
    Result<HostId> ResolvePreferredHost(const GlobalPointer& gptr) const;
    Result<SpscQueue*> GetQueue(HostId producer, HostId consumer);
    const RegionMapper& region_mapper() const { return region_mapper_; }

private:
    CxloomConfig config_;
    SharedRegionLayout layout_ {};
    RegionMapper region_mapper_;
    BootstrapHeader* bootstrap_ {nullptr};
    std::unique_ptr<GlobalAllocator> allocator_;
    std::unique_ptr<CoherenceManager> coherence_;
    std::vector<std::vector<std::unique_ptr<SpscQueue>>> queues_;
    bool initialized_ {false};

    Status InitializeBootstrap();
    Status AttachBootstrap();
    Status ValidateBootstrap(const BootstrapHeader& header) const;
    Status RegisterLocalHost();
};

}  // namespace cxloom::loommem
