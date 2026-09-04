# CXLoom

CXLoom is a research prototype for a software-coherent CXL shared-memory system and a distributed thread runtime built on top of it.

The codebase is split into two peer subsystems:

- `LoomMem`: shared-memory allocation, global addressing, software coherence, versioning, local replicas, and CXL-resident queues
- `LoomPar`: distributed thread create/join, placement, barrier, lifecycle management, and memory-aware execution

## Repository Layout

- `include/cxloom/common`: shared types, config, status, message definitions
- `include/cxloom/loommem`: LoomMem public interfaces
- `include/cxloom/loompar`: LoomPar public interfaces
- `src/common`: shared runtime helpers
- `src/loommem`: LoomMem implementations
- `src/loompar`: LoomPar implementations
- `examples`: small integration drivers
- `docs`: design and implementation notes

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Current Status

This repository currently contains a compile-ready architecture skeleton. Most distributed and coherence operations intentionally return `Unimplemented` so we can fill them in step by step while keeping module boundaries stable.

## C API

C applications include `cxloom/cxloom.h` and use the short `cl_` prefix. The first
usable C API is the standalone LoomMem allocation interface:

```c
#include <cxloom/cxloom.h>

cl_runtime_t *runtime = NULL;
cl_gptr_t object;
cl_runtime_create(&config, &runtime);
cl_mem_alloc(runtime, 4096, 64, &object);
cl_mem_free(runtime, object);
cl_runtime_destroy(runtime);
```

`cl_gptr_t` is a global offset-based handle, not a process-local virtual
address. Future `cl_mem_*_acquire/release` calls will resolve it safely for
read and write access under LoomMem's coherence rules.

## Shared Region Bootstrap

Set `cl_config_t.shared_region_path` to `/dev/dax0.0` on every logical host.
Exactly one host, normally host zero, sets `bootstrap_owner = 1`; all other
hosts attach with `bootstrap_owner = 0`. The owner publishes the layout in a
fixed bootstrap header at region offset zero, and attachers validate it before
using the mapping. `cl_mem_resolve_local` is available for mapping tests; it
does not provide coherence protection and must not replace future acquire/
release APIs.

## Multi-Host Initialization Test

A logical CXLoom host is a container, not a NUMA node. Containers are assigned
8 physical cores by default and NUMA-local DRAM (up to 32 GiB), and are spread
round-robin across the server's compute NUMA nodes. A 128-core, four-NUMA-node
server can therefore run up to 16 logical hosts.

After launching the containers, run a concurrent shared-DAX initialization test:

```bash
./scripts/run-host-init-containers.sh 16
```

Host zero creates a fresh bootstrap session and initializes one global append-only allocation pool. All hosts allocate from the same atomic bump cursor, publish one object, and then validate that every published range is non-overlapping and readable through each local mapping.

## Shared Allocator V1

For a shared DAX mapping, the bootstrap owner formats an allocator header in
the allocator region. Every host reserves space from one global atomic bump
cursor. A self-describing metadata prefix is stored immediately before each
aligned object and records its offset, size, alignment, owner, allocation ID,
and generation.

Shared allocation count has no per-host descriptor limit and is bounded only by
the shared-data pool capacity. Allocations are append-only: cl_mem_free returns CL_UNIMPLEMENTED for a DAX-backed runtime.
ResolveLocal accepts only published allocation base pointers in shared mode;
arbitrary offsets and interior pointers are rejected. The bootstrap object's
publication slots are bring-up/test coordination and are not a general-purpose
object directory.

## Visibility and Ordering Litmus

Run scripts/run-visibility-litmus-containers.sh after container launch to
compare release, sequentially consistent, CLFLUSH+MFENCE, and CLWB+SFENCE publication/acquisition recipes. See docs/visibility-ordering-litmus.md for the protocol and
interpretation rules. Only real /dev/dax0.0 results should determine the
runtime's eventual publication recipe.
