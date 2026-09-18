# CXLoom Design Understanding

This note records the current design and implementation contract:

- the CXL shared-memory design -> LoomMem
- the distributed execution design implemented by this repository -> LoomPar

Its purpose is to give us an implementation-oriented shared model before coding the full CXLoom system.

## 1. CXLoom: Unified System View

CXLoom consists of two peer subsystems:

- `LoomMem`: manages how shared data is represented, allocated, cached, synchronized, and made visible across hosts.
- `LoomPar`: manages how computation over that shared data is created, placed, synchronized, and completed across hosts.

The intended symmetry is:

- `Mem`: how shared data is managed
- `Par`: how computation over shared data is organized

Architecturally, the system is:

- a software-coherent CXL shared-memory substrate
- plus a distributed thread runtime built directly on top of it

## 2. LoomMem: What It Must Provide

From the LoomMem design document, the V1 system model is:

- One physical multi-NUMA server is partitioned into multiple isolated logical hosts.
- Each logical host has private CPU cores and local DRAM.
- All logical hosts map the same shared CXL memory region.
- Application-visible shared data lives in CXL-backed global memory.
- Each host may keep software-managed replicas of shared data in local DRAM.

Important caveat:

- This is a `multi-host CXL pod emulation platform`, not a true hardware multi-host non-coherent CXL pod.
- We must avoid relying on accidental hardware cache coherence inside the single machine.

Therefore the intended access discipline is:

- CXL shared memory is the authoritative global backing store.
- Host-local DRAM is a private replica/cache layer.
- All shared-memory accesses that matter semantically should go through the runtime, not arbitrary direct mutation.

## 3. LoomMem Core Mechanisms

### 3.1 Global Addressing

The design should not depend on host virtual addresses being identical.

The stable cross-host shared reference is:

- `GPtr = <region_id, offset>` or V1 simplification `GPtr = offset`

Each host resolves:

- `local_va = host_cxl_base + offset`

So the runtime contract is:

- applications and LoomPar use global pointers
- LoomMem resolves them to host-local mappings

### 3.2 CXL Memory Layout

The shared CXL region is logically divided into:

1. bootstrap/system metadata
2. global allocator metadata
3. global object/coherence metadata
4. CXL communication region
5. application shared data pool

This is the backbone for both LoomMem and LoomPar.

### 3.3 Global vs Local Metadata

This is one of the most important boundaries.

Global metadata in shared CXL memory:

- object/block offset and size
- allocation state
- per-block content version and writeback epoch
- per-block token owner
- global flags
- communication structures

Local metadata in each host's DRAM:

- replica local address
- cached / dirty state
- local block version
- local cache bookkeeping
- local token-related transient state

Rule:

- global truth goes in CXL
- host-private cache state stays local

### 3.4 Coherence Model

The design explicitly avoids software MESI as a V1 target.

V1 coherence is:

- `dynamic token ownership` for writer serialization
- `version-based freshness` for readers
- `release consistency` for synchronization semantics

Separation of concerns:

- token answers: who may perform the next exclusive mutation
- version answers: whether a reader's local replica is fresh enough

### 3.5 Dynamic Token Ownership

Per shared coherence block:

- there is exactly one token
- token ownership is exclusive
- token ownership is not data ownership

The token is a permission to execute the next mutable state transition.

The intended lifecycle is baton passing:

- allocator or first writer gets initial token
- current owner explicitly transfers token to the next owner
- avoid `NONE -> contender CAS race` as the normal path

This gives the core invariant:

- at any time, exactly one host is the legal writer of a block

### 3.6 Version-Based Reader Freshness

Readers do not need the token.

Expected flow:

- on read, use local replica if present
- compare the cached and shared versions of the same block
- if equal, read locally
- if stale, invalidate/reload from CXL and refresh local version

This is intentionally lightweight for read-mostly behavior.

### 3.7 Release Consistency Semantics

The document strongly prefers:

- `release consistency for DRF programs`

Instead of:

- per-write immediate global invalidation
- sequential consistency everywhere

The expected semantic shape is:

- ordinary reads/writes use local replicas
- release or barrier publishes writes
- acquire or barrier validates visibility/version

This aligns naturally with LoomPar barriers.

### 3.8 CXL-Resident Control Plane

The current direction uses the shared CXL region for the steady-state control plane.

CXL should carry:

- shared application data
- coherence metadata
- token-transfer coordination
- thread lifecycle control
- barrier control traffic

The control primitive is:

- one directed `CXL-resident SPSC queue` per distinct host pair
- producer and consumer cursors on separate single-writer cache lines
- fixed inline control payloads, with larger data referenced through GPtr
- one CPU-bound consumer poller per host with rotating scans and bounded batch drains

Reason:

- fixed producer / fixed consumer
- avoids cross-host MPMC CAS/FAA complexity
- supports a single-writer discipline for mutable control fields

### 3.9 Ordering Requirements

Token transfer correctness depends on strict publication order:

1. finish dirty local data
2. write back / publish to CXL
3. execute visibility fence
4. publish new version / metadata
5. publish new token owner
6. send token grant

This is a hard implementation dependency and should be tested before full runtime work.

### 3.10 Granularity and Allocation

Coherence granularity should be configurable, not hard-coded.

Candidates include:

- `64B`
- `256B`
- `1KB`
- `4KB`
- `16KB`

Objects define allocation and reclamation lifetimes. Fixed-size blocks define
coherence and caching, with `4KB` as the default starting point. Applications
choose object boundaries. Multi-block operations provide per-block consistency;
applications coordinate any cross-block invariants.

Allocator design is separated from coherence-token design. The current shared
allocator keeps address-ordered extent indexes in allocator metadata, with
independent pools for object data and dense block sidecars. Allocation splits
free extents; retirement coalesces adjacent ranges. The inline descriptor exists
only during the object's lifetime and does not change the pure-address GPtr
contract.

## 4. LoomPar Execution Model

The LoomPar execution layer keeps the data plane and computation plane separate:

- LoomMem owns shared data, coherence and visibility
- LoomPar owns cross-host invocation, placement and lifecycle

LoomPar is intentionally:

- thread-centric
- Pthreads-like
- distributed
- pinned, not migratory

It does not move live stacks or continuations.

## 5. LoomPar Core Mechanisms

### 5.1 Programming Model

The public API is an opaque C interface:

- `cl_runtime_create` / `cl_runtime_destroy`
- `cl_pthread_create` / `cl_pthread_join` / `cl_pthread_detach`
- `cl_pthread_create_with_working_set`
- reusable world barriers and distributed mutex/condition handles

The C++ runtime and scheduler are internal integration interfaces. Multi-host
callbacks use an immutable cluster function manifest; bounded argument bytes are
serialized into queue messages and large shared data is passed by GPtr.

### 5.2 Pinned Cross-Host Threads

Each created thread:

- may execute locally or on a remote host
- gets a cluster-wide identity
- runs as a native local thread on the selected execution host
- stays pinned there until completion

This means LoomPar is not a task-migration framework.

The only thing that moves is the launch decision, not live execution state.

### 5.3 Home Rank and Global Thread Identity

LoomPar uses `GTID = <home_host, local_tid>`:

Semantically:

- `home rank`: the creator-side lifecycle owner
- `execution rank`: where the native thread actually runs

The home side keeps authoritative lifecycle metadata.
The execution side keeps only transient execution records.

The home host owns lifecycle metadata; the execution host keeps transient records.

### 5.4 Remote Launch by Function Registration

LoomPar does not ship code or stack state. Instead:

- functions are registered cluster-wide
- a remote create sends compact metadata:
  - `GTID`
  - `func_id`
  - argument payload

The remote side:

- resolves `func_id`
- reconstructs local invocation
- launches a native thread

The remote side resolves the local callback binding and launches a pinned Fiber.

### 5.5 Lifecycle State Machine

The home-side lifecycle states are:

- `ALLOCATED`
- `LAUNCHING`
- `RUNNING`
- `COMPLETED`
- `JOINED`

Remote execution flow:

1. creator allocates GTID and home metadata
2. scheduler picks execution host
3. create request is sent
4. target host launches native thread
5. target sends create ack
6. thread runs and finishes
7. target sends complete notification
8. home host marks completed
9. join observes completion and reclaims metadata

This state machine should remain central in LoomPar.

### 5.6 Placement Scheduler

LoomPar schedules at create time. The configured policies are:

- memory-aware placement (the default)
- round-robin over eligible hosts
- least-loaded placement using execution telemetry

Memory-aware placement is based on:

- the declared GPtr working set
- token owner, last writer and current-version replica residency
- executing, ready and blocked Fiber counts
- queue pressure, admission limits and launch history

Working-set read/write types are user hints at create time; LoomMem remains the
authority for actual access and consistency.

### 5.7 Barrier Semantics

LoomPar's barrier is:

- a distributed phase-boundary primitive
- separate from ordinary data-access semantics
- responsible for aligning thread progress
- coupled to LoomMem release/acquire hooks so published writes become visible at the phase boundary

This matches LoomMem's release-consistency design almost exactly.

## 6. LoomPar Control Plane

LoomPar uses LoomMem's shared CXL region for both data and control. Directed
SPSC queues carry:

- `CREATE_REQ`
- `CREATE_ACK`
- `COMPLETE_NOTIFY`
- `JOIN_WAKE` or equivalent local completion path
- `BARRIER_ARRIVE`
- `BARRIER_RELEASE`
- token/coherence messages such as `TOKEN_REQ` and `TOKEN_GRANT`

## 7. LoomMem <-> LoomPar Contract

This is the implemented co-design contract.

LoomMem should provide LoomPar with:

- global pointer representation and translation
- shared allocation
- CXL-resident communication queues
- versioned read/write primitives or lower-level acquire/release hooks
- barrier-related completion/visibility hooks
- locality hints:
  - token owner
  - last writer
  - current-version replica residency
  - dominant object/block placement

LoomPar should provide LoomMem with:

- thread creation context
- expected working-set hints for placement
- synchronization boundaries
- execution-host decisions that may affect data movement pressure

The division of responsibility is:

- LoomMem knows where data state currently lives and how expensive it is to move coherence
- LoomPar decides where to run threads so execution follows favorable memory/coherence state

## 8. Current Co-Design Boundary

LoomPar moves a new invocation toward the currently favorable coherence state,
without moving a live stack or changing ownership merely to satisfy placement.
It can use:

- current token owner
- a replica with the current published version
- last writer
- execution load and admission state

Replica access heat, hardware latency calibration and a learned cost model are
explicitly deferred. The current cost is a deterministic heuristic used for
placement, not a measured performance model.

## 9. Practical V1 Boundaries

The two documents together suggest a realistic first implementation target:

- multi-NUMA logical-host emulation on one machine
- shared CXL region abstraction
- global offset-based addressing
- shared split/coalesce extent allocator with ephemeral inline descriptors
- host-local replica cache
- dynamic token ownership per block
- version-based reader freshness
- release-consistency synchronization
- per-host-pair SPSC CXL queues
- distributed thread create/join/barrier on top of those queues
- initial placement policy using execution load plus current LoomMem state

What should not be overcommitted in V1:

- true hardware multi-host non-coherence claims
- full software MESI
- fully dynamic migrating execution
- hard-coded coherence granularity
- assuming specific CXL visibility primitives before measurement

## 10. Current Status and Remaining Work

The implementation has completed the core LoomPar path: local and remote
create, cluster function registration, GTID home ownership, result-bearing join,
detach reclamation, pinned Fiber execution, cooperative join/barrier/mutex/
condition/token waits, release/acquire integration, execution-load telemetry,
and memory-aware, round-robin and least-loaded placement. LoomMem now exposes
per-block token owner, last writer and current-version replica residency.

The remaining gaps are deliberately bounded:

1. Host failure detection, failed remote invocation convergence and transparent
   recovery are not implemented.
2. Cancellation and deadline propagation for a running distributed invocation
   are not implemented; timed waits only cover supported LoomPar wait points.
3. Barrier membership is fixed per barrier ID. Dynamic cohorts, missing-host
   recovery and barrier cancellation are not implemented.
4. Arbitrary blocking system calls inside a Fiber still occupy a worker; only
   LoomPar/LoomMem cooperative waits park the Fiber.
5. Physical non-coherent multi-host CXL/DAX acceptance and performance
   characterization remain pending. File-backed multi-process and container
   tests validate protocol behavior, not hardware latency.
6. Application benchmarks, token-transfer counters, queue latency, telemetry
   ablations and granularity trade-offs remain to be measured.
7. Automatic discovery of a callback's future working set, replica heat
   tracking and calibrated/learned placement costs are deferred by design.

Live-stack migration is not a remaining V1 requirement: LoomPar's current
contract places an invocation at creation and keeps it pinned until completion.
The migration transaction model is retained as an experimental future extension,
but the runtime correctly rejects migration requests as unsupported.

This order is important because ordering and visibility semantics constrain everything above them.

## 11. Proposed Internal Naming

To keep the system clean in code, a good internal split is:

- `loommem/`
  - shared region bootstrap
  - allocator
  - gptr/addressing
  - metadata
  - replica cache
  - coherence/token/version
  - cxl queue transport

- `loompar/`
  - API
  - thread table / GTID
  - dispatcher
  - function registry
  - scheduler
  - barrier
  - execution runtime

- `common/`
  - config
  - host/rank IDs
  - message formats
  - tracing
  - platform abstractions

## 12. Summary

CXLoom should be implemented as a two-layer co-designed runtime:

- `LoomMem` is the software-coherent shared-memory substrate over CXL, with global metadata in CXL, private replicas in host DRAM, dynamic-token write serialization, version-based reader freshness, release consistency, and CXL-resident SPSC queues.
- `LoomPar` is the distributed thread runtime above LoomMem, preserving Pthreads-like create/join/barrier semantics, using home-owned global thread lifecycle management, metadata-only remote launch, pinned execution, and placement guided by load plus memory/coherence locality.

The most important architectural decision is that CXLoom should not treat memory and execution as separate afterthoughts. LoomMem exposes coherence state, and LoomPar should use that state to place threads where the total execution plus coherence cost is lowest.

## 13. Validation Scope

The reproducible acceptance workload uses 16 independent processes or containers
with a shared file-backed region. It covers remote argument delivery, concurrent
creators, nested cross-host create/join, result and detach reclamation, barriers,
conditions, execution-load telemetry, and all three placement policies. This is
protocol validation; it is not a claim of physical DAX or CXL performance.

## 14. Synchronization implementation contract

The C++ LoomPar runtime now implements a reusable blocking world barrier, with
fixed positive local participant counts per barrier ID (counts can differ across
hosts), coordinator host 0, explicit generations and local condition-variable
waiters. All configured hosts must arrive. Duplicate arrivals count once and stale
round messages cannot release a later round. Membership subsets, missing-host
recovery and dynamic participant registration are not part of this version.

LoomMem supplies SynchronizeRelease/SynchronizeAcquire hooks at create dispatch,
worker entry/completion, join completion and barrier arrival/return. Its existing
ReleaseWriteBuffer is still an immediate commit. StageWriteBuffer explicitly
transfers a finished mutable buffer to the current thread's next release boundary;
raw outstanding buffers are not silently committed. Acquire clears reusable local
replicas but leaves previously returned immutable snapshots unchanged. Applications
must reacquire snapshots to observe later publications.

See [the detailed synchronization contract](loompar-synchronization.md) for
ownership, failure behavior, participant rules and the 16-host validation workload.
Physical non-coherent visibility validation remains deferred.

## 15. Application API boundary

The public LoomPar programming model is an opaque C interface with create,
working-set create, join, detach, barrier, mutex, condition and memory APIs.
Applications provide a working-set hint only when they want memory-aware
placement; they do not provide control messages or remote callback addresses.
The runtime resolves the immutable function manifest, serializes bounded
arguments, selects a host, performs admission and queue transport, manages
GTIDs, and applies LoomMem synchronization hooks. The C++ LoomPar classes remain
internal implementation and test interfaces.
