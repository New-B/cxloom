#include <iostream>
#include <vector>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"
#include "cxloom/loompar/runtime.h"

int main() {
    cxloom::CxloomConfig config;
    config.host_count = 4;
    config.local_host_id = 0;
    config.shared_region_bytes = 512ULL << 20;

    cxloom::loommem::LoomMemRuntime loommem(config);
    auto mem_status = loommem.Initialize();
    if (!mem_status.ok()) {
        std::cerr << "LoomMem init failed: " << mem_status.message() << "\n";
        return 1;
    }

    auto alloc = loommem.AllocateShared(4096, 4096);
    if (!alloc.ok()) {
        std::cerr << "AllocateShared failed: " << alloc.status().message() << "\n";
        return 1;
    }

    cxloom::loompar::LoomParRuntime loompar(config, &loommem);
    auto par_status = loompar.Initialize();
    if (!par_status.ok()) {
        std::cerr << "LoomPar init failed: " << par_status.message() << "\n";
        return 1;
    }

    cxloom::ThreadPlacementHint hint;
    hint.has_dominant_gptr = true;
    hint.dominant_gptr = alloc.value();
    hint.label = "smoke-thread";

    auto thread = loompar.CreateThread("smoke_worker", std::vector<std::byte>{}, hint);
    std::cout << "shared object at offset " << alloc.value().offset << "\n";
    if (!thread.ok()) {
        std::cout << "thread creation is expectedly incomplete in the skeleton: "
                  << thread.status().message() << "\n";
    }

    auto finalize_status = loompar.Finalize();
    if (!finalize_status.ok()) {
        std::cerr << "LoomPar finalize failed: " << finalize_status.message() << "\n";
        return 1;
    }

    auto mem_finalize = loommem.Finalize();
    if (!mem_finalize.ok()) {
        std::cerr << "LoomMem finalize failed: " << mem_finalize.message() << "\n";
        return 1;
    }

    return 0;
}

