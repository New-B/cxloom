# LoomPar execution contract — 2026-09-18

This document is the current execution contract. Earlier milestone wording that
described LoomPar as only a local/native-thread prototype has been removed.

## Implemented contract

- Local and cluster registration are separate. Cluster registration installs an
  immutable manifest on every host with stable function ID, ABI version,
  argument size and schema ID. Missing, incompatible or reordered descriptors
  fail before remote create.
- Creates allocate `GTID = <home_host, local_tid>` at the home host. The selected
  host launches a pinned LoomPar Fiber and retains it until completion. Live
  stacks and continuations are not migrated.
- Remote messages carry a fixed-width prefix and at most 80 inline argument
  bytes. Larger shared data is represented by application-owned GPtrs. The wire
  contract assumes compatible endianness and structure layout; LoomPar does not
  transfer ownership of referenced allocations.
- The home lifecycle is `ALLOCATED -> LAUNCHING -> RUNNING -> COMPLETED ->
  JOINED`. Remote execution records are temporary and are reclaimed before the
  completion notification is delivered to the home host.
- Join returns the callback result and reclaims the home record. Detach defers
  reclamation until completion. Exceptions and remote launch failures become a
  failed join while still releasing lifecycle state. Self-join, foreign IDs,
  unknown IDs and duplicate joiners are rejected.
- LoomPar owns the LoomMem inbound poller in multi-host mode. A local progress
  thread reaps execution records and retries queued lifecycle traffic without
  blocking the inbound poller. Token messages continue to dispatch to LoomMem.
- Join, barrier, mutex, condition and token waits park a Fiber cooperatively;
  parked Fibers do not consume a worker. Arbitrary blocking system calls still
  occupy their worker.
- `cl_pthread_create_with_working_set` accepts GPtr ranges, read/write hints and
  weights. Hints are used only by memory-aware placement; actual reads and
  writes remain governed by LoomMem.
- Execution telemetry distinguishes `executing`, `ready`, `blocked` and
  `workers`. The runtime publishes versioned load samples periodically and on
  lifecycle changes. A least-loaded scheduler uses normalized runnable demand;
  round-robin and memory-aware policies are also available.
- Finalize requires a coordinated quiescent phase. It rejects unjoined home
  records, active remote records, active barriers and staged writes. It is not a
  distributed shutdown barrier.

## Placement contract

`CL_PLACEMENT_MEMORY_AWARE` is the default. It combines execution load, queue
pressure, admission limits, launch history and current LoomMem coherence state:
token owner, last writer and current-version replica residency.
`CL_PLACEMENT_ROUND_ROBIN` selects the next eligible host.
`CL_PLACEMENT_LEAST_LOADED` selects the lowest normalized runnable load and
accounts for launches not yet visible in remote telemetry.

Replica heat tracking and a measured or learned memory-cost model are deferred.
The current coherence term is a deterministic placement heuristic, not a
hardware latency measurement. A working-set read/write type is an application
hint because the callback has not executed when placement occurs; LoomMem still
checks every actual access.

## Validation

The current Debug build passes 38 CTest tests. Coverage includes LoomMem
allocation, token and block coherence, C API, Fiber scheduling, local and remote
LoomPar lifecycle, cluster registration, barriers, distributed synchronization,
working-set locality, execution-load accounting and all three placement policies.
The 16-host process scenarios verify copied arguments, nested remote create/join,
result values, detach reclamation, queue backpressure and zero remaining home
records.

File-backed shared mappings validate protocol behavior. They do not establish
physical non-coherent CXL/DAX visibility or performance.

## Remaining work

Host failure detection, failed invocation convergence, transparent recovery,
cancellation and deadline propagation are not implemented. Barrier membership
is fixed per barrier ID; dynamic cohorts and missing-host recovery are not
implemented. Physical CXL acceptance, application benchmarks, latency/counter
instrumentation, granularity studies and systematic fault injection remain
Phase 9 work.

Running-thread migration is deliberately outside the pinned V1 contract. The
repository retains an experimental migration transaction model for future work,
but runtime migration requests are rejected as unsupported and no successful
continuation transfer is claimed.
