# CXLoom

CXLoom is a research prototype for a software-coherent CXL shared-memory system and a distributed thread runtime built on top of it.

The codebase is split into two peer subsystems:

- `LoomMem`: shared-memory allocation, global addressing, software coherence, versioning, local replicas, and CXL-resident queues
- `LoomPar`: distributed thread create/join, placement, barrier, lifecycle management, and memory-aware execution

Applications use the opaque C `cl_pthread_*` interface for thread creation,
join and barriers. The runtime chooses placement, load admission, queue
transport and execution hosts internally. The lower-level C++ LoomPar classes
are implementation and test interfaces, not the application programming model.

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

The current Debug build passes 38 CTest tests. LoomPar supports cluster
function registration, local and remote create/join/detach with return values,
pinned Fiber execution, cooperative waits, barriers, distributed mutexes and
conditions, and LoomMem release/acquire boundaries. Placement supports the
default memory-aware policy plus round-robin and least-loaded policies, with
execution telemetry for executing, ready and blocked Fibers. Working-set read /
write information is an application hint used only by memory-aware placement.

The 16-host file-backed process and container harnesses validate protocol
behavior and lifecycle reclamation. They do not establish physical non-coherent
CXL/DAX visibility or performance. Remaining work is host failure recovery,
cancellation/deadlines, dynamic barrier membership, arbitrary blocking-call
isolation, physical CXL acceptance, and performance/application evaluation. See
[the execution contract](docs/loompar-execution-milestone.md), [the design and
remaining gaps](docs/cxloom-design-understanding.md), and [the synchronization
contract](docs/loompar-synchronization.md).

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

`cl_gptr_t` is a global offset-based handle. Applications access shared data
through `cl_mem_read` and `cl_mem_write` under LoomMem's block-level coherence
rules. LoomMem manages only global shared CXL memory; applications allocate
private DRAM using standard system or language allocation APIs.

## Shared Region Bootstrap

`shared_region_path` is required. Set it to `/dev/dax0.0` on every logical
host for CXL operation. Tests may provide a regular file, mapped with
`MAP_SHARED`, to exercise the same allocator and protocol without CXL hardware.
Exactly one host, normally host zero, sets `bootstrap_owner = 1`; all other
hosts attach with `bootstrap_owner = 0`. The owner publishes the layout in a
fixed bootstrap header at region offset zero, and attachers validate it before
using the mapping. `cl_mem_resolve_local` is available for mapping tests; it
does not provide coherence protection and must not replace the read/write APIs.

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

## Shared Allocator V2 (TLSF)

For a shared DAX mapping, the bootstrap owner formats an allocator header in
the allocator region. TLSF-style shared size bins select free ranges without
scanning the complete free list; an address-ordered index still supports
splitting and coalescing. A self-describing metadata prefix exists
immediately before an object only while that object is allocated.

Shared allocation count has no per-host descriptor limit and is bounded by the
shared-data and coherence-metadata regions. `cl_mem_free` retires and reuses an
entire object after preventing new acquires and waiting for every host's active
references and all writebacks to drain. Its data and sidecar extents then
return independently to their free pools only after the all-host retirement
protocol certifies that caches, accepted operations, and queue watermarks are
drained. The object address is invalid immediately after free; using it again
is an application error.
ResolveLocal accepts only published allocation base pointers;
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
./scripts/launch-numa-containers.sh 16
./scripts/run-token-stress-containers.sh
```

`CL_HOST_COUNT=16 ./scripts/launch-numa-containers.sh` is equivalent. Explicit
test-script arguments or environment variables override the discovered value.
When `queue_capacity_entries` is zero (the default), LoomMem chooses the largest
per-pair capacity up to 1024 that fits all `N * (N - 1)` directed queues in the
reserved queue region. Explicit capacities remain supported and are rejected
if they do not fit.

## Single-Writer/Multi-Reader Coherence

The public C++ memory API is collected in `cxloom/loommem.h`. Applications use
`clInit`/`clDestroy`, `clAlloc`, `clRead`/`clReadRange`,
`clWrite`/`clWriteRange`, and `clFree`. `ReadView` owns an immutable snapshot;
`WriteView` exposes `data()`, `Commit()`, `Abort()`, and `Stage()`, and automatically
aborts an active view on destruction. Runtime polling, token transfer,
references, descriptors, and sidecars remain internal to this API.

`WriteView::Stage()` transfers the buffer to the runtime without publishing it.
On success the view is inactive (`data() == nullptr`, `size() == 0`); destroying
or moving over it does not abort the staged write. The staging thread's next
`clSynchronizeRelease(context)` publishes all its staged writes in order. Other
threads' release calls and acquire boundaries do not publish them. Stop using
all mutable pointers after staging, and release before the staging thread exits
or the context is destroyed. Staged writes retain their tokens and delay object
reclamation. A failed Stage leaves an active view's ownership unchanged.


Objects are the allocation, addressing, and reclamation unit. Fixed-size
coherence blocks within each object are independent token, version, writeback,
and replica-LRU units. The default block size is 4 KiB, with per-allocation
block-size overrides. Small objects naturally occupy one block.

`AcquireReadRange` and `AcquireWriteRange` accept byte ranges. Multi-block
writers acquire tokens in ascending block order and publish each block
independently. Readers validate each block's version and writeback epoch.
`AcquireReadSnapshot` and `AcquireWriteBuffer`, like `clRead` and `clWrite`,
cover the full object range with these same per-block guarantees. Multi-block
reads are immutable results, not point-in-time snapshots of the object;
applications synchronize cross-block invariants.

See `docs/cxl-coherence.md` for the protocol and
`docs/coherence-block-design.md` for metadata, range semantics, and lifecycle
management. `docs/object-retirement.md` specifies the all-host closing,
watermark draining, cleaning, and reclaimable phases. The current shared layout
requires reinitializing older regions
(bootstrap version 12, allocator version 12). The stable descriptor directory
supports up to 2,048 live/retiring objects per region.

Reads use release consistency: current-interval cache hits use local DRAM,
while `clSynchronizeAcquire` rotates two replica indexes and validates old
blocks lazily on their next access. `clInvalidate` explicitly drops local cached
blocks. Replica references count hosts, independently of in-flight CXL operation
pins. See [the read-access protocol](docs/read-access-design.md).

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

Multi-host C API applications now register a collective function manifest with
`cl_pthread_register_functions` before creating threads. Names, declared ABI/schema
and argument sizes are checked across hosts; private callbacks resolve through
stable IDs without exporting symbols. See [the API contract](docs/loompar-user-api.md#cluster-function-registration)
and `cxloom_loompar_cluster_process` for automatic remote placement, return values,
nested create/join and detached invocation reclamation.
