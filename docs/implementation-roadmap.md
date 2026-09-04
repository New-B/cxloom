# CXLoom Implementation Roadmap

This roadmap assumes we keep the current two-subsystem split:

- `LoomMem`: shared-memory substrate
- `LoomPar`: distributed thread runtime

The order below is intentionally bottom-up. Higher layers should not be implemented before the visibility and control assumptions underneath them are validated.

## Phase 0: Platform Bring-Up

Goal:

- establish the multi-NUMA logical-host emulation environment
- confirm process/container pinning and shared CXL mapping assumptions

Tasks:

1. define host abstraction, host IDs, and per-host config
2. map the shared CXL region into every logical host
3. verify offset-based address translation works correctly across hosts
4. add tracing, asserts, and failure-reporting infrastructure

Exit criteria:

- every host can resolve the same `GPtr` to its own local mapping
- bootstrap metadata is visible to all hosts

## Phase 1: Visibility and Ordering Litmus Tests

Goal:

- determine the real visibility/fence/flush rules needed on the platform

Tasks:

1. implement a small two-host visibility test harness
2. test ordinary loads after remote writes
3. test writeback and fence combinations
4. test control-word visibility separately from data visibility
5. record the minimum correct ordering recipe for token handoff

Exit criteria:

- we know the required `data write -> flush -> fence -> metadata publish -> handoff` sequence
- we stop making unsupported assumptions about CXL visibility

## Phase 2: Shared Region Bootstrap and Layout

Goal:

- make LoomMem own a stable on-CXL metadata layout

Tasks:

1. implement bootstrap/system metadata
2. finalize allocator/coherence/queue/shared-data region boundaries
3. encode layout versioning and compatibility checks
4. expose `GPtr` helpers and local resolution helpers

Exit criteria:

- all hosts agree on one region layout
- metadata structures can be discovered from bootstrap state alone

## Phase 3: CXL Queue Transport

Goal:

- bring up the CXL-only control plane

Tasks:

1. implement per-host-pair SPSC ring buffers in shared memory
2. separate producer and consumer indices to preserve single-writer ownership
3. add message headers, sequence checks, and polling helpers
4. validate queue ordering and backpressure behavior

Exit criteria:

- one host can reliably send and receive control messages through shared CXL memory
- queue correctness does not depend on cross-host CAS-heavy structures

Current status:

- implemented one CXL-resident SPSC ring per directed non-self host pair
- separated producer tail and consumer head into single-writer cache lines
- added fixed slots, sequence validation, backpressure, endpoint checks, and visibility-profile integration
- added a CPU-bound round-robin poller with batch drain, adaptive backoff, dispatch callbacks, and runtime lifecycle management
- validated all 12 directed queues with four NUMA-pinned hosts on `/dev/dax0.0`

## Phase 4: LoomMem Allocator and Global Addressing

Goal:

- make shared allocation usable by upper layers

Tasks:

1. initialize an append-only global shared-data pool
2. reserve aligned object space with a shared atomic cursor
3. store allocation metadata inline before each object
4. validate and resolve allocation-base GPtrs across hosts
5. reserve future metadata semantics for deallocation and reuse

Exit criteria:

- shared objects can be allocated and referenced by GPtr
- allocation capacity is bounded by the global pool rather than a per-host quota
- allocation descriptors can be queried from every host

## Phase 5: LoomMem Coherence V1

Goal:

- deliver the first correct software coherence protocol

Tasks:

1. define object/block metadata structures
2. implement token ownership and transfer protocol
3. implement local replica metadata
4. implement version-based reader validation
5. implement release-side publish ordering
6. integrate token and queue messaging

Exit criteria:

- one writer at a time is enforced
- stale readers are detected and refreshed
- release consistency semantics can be built on top

## Phase 6: LoomPar Lifecycle Control

Goal:

- implement the distributed thread abstraction independent of application logic

Tasks:

1. implement `GTID=<home_host, local_tid>`
2. implement home-owned thread table and lifecycle states
3. implement function registration
4. implement `CREATE_REQ`, `CREATE_ACK`, and `COMPLETE_NOTIFY`
5. implement local dispatcher and native-thread launch wrapper

Exit criteria:

- remote create and completion work over CXL queues
- lifecycle authority remains at the home host

## Phase 7: Join and Barrier

Goal:

- make synchronization semantics complete enough for real programs

Tasks:

1. implement blocking and wakeup for `join`
2. implement generation-based barrier
3. make barrier delegate issue LoomMem completion hooks before arrival
4. validate release/acquire behavior at synchronization boundaries

Exit criteria:

- create/join/barrier form a coherent runtime contract
- LoomPar synchronization composes correctly with LoomMem consistency

## Phase 8: Placement and Memory-Execution Co-Design

Goal:

- use LoomMem state to improve LoomPar scheduling

Tasks:

1. start with round-robin and least-loaded baselines
2. add dominant-object placement hints
3. expose token owner, last writer, and replica residency from LoomMem
4. define a placement cost model using load plus coherence cost
5. compare `move thread` versus `move token/data`

Exit criteria:

- LoomPar placement uses actual memory/coherence signals
- the system begins to realize the main CXLoom research idea

## Phase 9: Benchmarks and Hardening

Goal:

- move from correct prototype to publishable system

Tasks:

1. add microbenchmarks for allocator, queue, token transfer, create/join, and barrier
2. port initial applications
3. add failure injection and debug counters
4. measure granularity tradeoffs
5. refine metadata footprint and fast paths

Exit criteria:

- the system is stable enough for larger experiments
- each subsystem has clear performance and correctness evidence

## Recommended Coding Order Inside the Current Skeleton

If we use the current repository skeleton, the best next implementation order is:

1. `src/loommem/runtime.cpp`
2. `src/loommem/queue.cpp`
3. `src/loommem/allocator.cpp`
4. `src/loommem/coherence.cpp`
5. `src/loompar/threading.cpp`
6. `src/loompar/runtime.cpp`
7. `src/loompar/barrier.cpp`
8. `src/loompar/scheduler.cpp`

That order keeps the control plane from getting ahead of the data-plane guarantees it depends on.

## Current Shared Allocator V1 Boundary

The initial Phase 4 slice now uses owner-formatted allocator metadata in the
shared CXL region. It provides a global atomic bump pool, stable offset-based global pointers, inline allocation descriptors without a per-host record limit, allocation and owner lookup, and multi-host read-only bring-up tests.

This V1 is intentionally append-only. Reclamation, generation reuse, remote
lifetime tracking, and a general object directory remain future work. The next
platform step after validating this allocator on real devdax is the now-available visibility
and ordering litmus suite used to replace generic C++ release/acquire
assumptions with a measured CXL publication recipe.
