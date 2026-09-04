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

Host zero creates a fresh bootstrap session and initializes independent shared
extent pools for object data and coherence sidecars.

## Shared Allocator V1

For a shared DAX mapping, the bootstrap owner formats an allocator header in
the allocator region. A shared, address-ordered extent index allocates, splits,
returns, and coalesces free ranges. A self-describing metadata prefix exists
immediately before an object only while that object is allocated.

Shared allocation count has no per-host descriptor limit and is bounded by the
shared-data and coherence-metadata regions. `cl_mem_free` retires and reuses an
entire object after preventing new acquires and waiting for every host's active
references and all writebacks to drain. Its data and sidecar extents then
return independently to their free pools; a later allocation creates a new
descriptor and a new allocation ID.
ResolveLocal accepts only published allocation base pointers in shared mode;
arbitrary offsets and interior pointers are rejected. The bootstrap object's
publication slots are bring-up/test coordination and are not a general-purpose
object directory.

`GPtr` remains a pure shared address. It becomes invalid immediately after
free, and using it afterward is a caller error. Formal access uses LoomMem
read/write acquire APIs or an explicit `ObjectReference`; `ResolveLocal` is
restricted to bootstrap and mapping diagnostics.

## CXL-Resident SPSC Queue Transport

Every directed pair of distinct hosts owns one fixed-capacity ring in the
shared CXL queue region. Each host can run a CPU-bound round-robin poller with
batched draining and adaptive idle backoff. Run the variable-scale all-pairs transport test with:

```bash
./scripts/run-queue-transport-containers.sh
```

See `docs/cxl-spsc-queue.md` for the shared layout, ordering protocol, capacity
constraints and validation procedure.

## Queue-Based Write Tokens

Shared allocations carry an authoritative owner, version, and token epoch.
`RequestWriteToken`, `WaitForWriteToken`, and `ReleaseWriteToken` use the
host-pair queues and the dedicated poller to serialize writers and publish data
before ownership transfer. See `docs/cxl-token-protocol.md` for the state
machine and cross-queue ordering rules.

Choose the host count once at container startup. Runtime queue matrices, token
pollers, and subsequent test scripts inherit that count automatically:

```bash
./scripts/launch-numa-containers.sh 12
./scripts/run-token-stress-containers.sh
```

`CL_HOST_COUNT=12 ./scripts/launch-numa-containers.sh` is equivalent. Explicit
test-script arguments or environment variables override the discovered value.
When `queue_capacity_entries` is zero (the default), LoomMem chooses the largest
per-pair capacity up to 1024 that fits all `N * (N - 1)` directed queues in the
reserved queue region. Explicit capacities remain supported and are rejected
if they do not fit.

## Single-Writer/Multi-Reader Coherence

The public C++ memory API is collected in `cxloom/loommem.h`. Applications use
`clInit`/`clDestroy`, `clAlloc`, `clRead`/`clReadRange`,
`clWrite`/`clWriteRange`, and `clFree`. `ReadView` owns an immutable snapshot;
`WriteView` exposes `data()`, `Commit()`, and `Abort()`, and automatically
aborts an active view on destruction. Runtime polling, token transfer,
references, descriptors, and sidecars remain internal to this API.

`AcquireWriteBuffer` and `ReleaseWriteBuffer` combine token ownership with
version publication. `AcquireReadSnapshot` maintains an immutable host-local
replica and refreshes it when the shared version advances. A descriptor
coherence epoch prevents readers from accepting a concurrent partial
writeback. See `docs/cxl-coherence.md`.

The next coherence-granularity evolution separates allocation identity from
block-level token, version, writeback, and replica state. The proposed metadata
layout, range semantics, atomicity modes, allocator integration, and migration
plan are specified in `docs/coherence-block-design.md`.

Objects remain the allocation and reclamation unit. Within each object,
configurable coherence blocks are independent token, version, writeback, and
replica-LRU units. `AcquireReadRange` and `AcquireWriteRange` accept byte ranges;
multi-block writers acquire tokens in ascending block order. The existing
whole-object snapshot and write-buffer APIs wrap the full object range.

Run the variable-scale devdax validation with:

```bash
./scripts/launch-numa-containers.sh <host-count>
./scripts/run-coherence-stress-containers.sh
```

## Visibility and Ordering Litmus

Run scripts/run-visibility-litmus-containers.sh after container launch to
compare release, sequentially consistent, CLFLUSH+MFENCE, and CLWB+SFENCE publication/acquisition recipes. See docs/visibility-ordering-litmus.md for the protocol and
interpretation rules. Only real /dev/dax0.0 results should determine the
runtime's eventual publication recipe.
