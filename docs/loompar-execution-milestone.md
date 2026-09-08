# LoomPar execution milestone — 2026-09-05

## Implemented contract

- `RegisterFunction(name, void (*)(void*))` requires a nonempty name and nonnull
  function. Names produce stable 64-bit FNV-1a IDs independently of registration
  order. Conflicting bindings and locally detected hash collisions are rejected.
  Participants must use compatible binaries, names and argument schemas and
  register functions before advertising readiness. Cluster-wide schema/version
  negotiation remains future work.
- `CreateThread` uses the existing placement interface and allocates
  `<home_host, local_tid>` at home. A native `std::thread` executes the function
  on its selected host. Local argument storage is owned by that invocation.
  Cross-process pointers must be represented as GPtr values and resolved locally.
- Remote messages contain a 24-byte fixed-width prefix and up to 104 bytes of
  inline arguments. Larger arguments must be represented by an application-owned
  GPtr. The current wire ABI assumes homogeneous endianness and structure layout.
  There is no automatic ownership transfer of referenced shared allocations.
- `JoinThread` waits on a local condition variable until completion, joins the
  native worker where applicable, and removes the home record. It rejects foreign
  home IDs, unknown IDs, self-join and concurrent duplicate joiners. A second join
  after reclamation returns NotFound. Worker exceptions and remote launch failures
  become a failed join; that join still reclaims the record. No return-value API,
  detach or cancellation is introduced in this milestone.
- Local and remote home records use ALLOCATED → LAUNCHING → RUNNING → COMPLETED
  → JOINED. Launch failures can complete directly from LAUNCHING. Delayed ACKs
  cannot undo completion. Execution hosts keep separate temporary records, join
  their native workers and reclaim them before queuing completion to home.
- LoomPar owns the LoomMem inbound poller in multi-host mode. Token messages still
  dispatch to LoomMem. A local progress thread reaps execution records and retries
  queued outbound traffic without blocking the inbound poller. Queue Push is
  serialized within the host's shared queue wrapper, including token producers.
  Do not construct competing wrappers or start another consumer for these queues.
- `Finalize` rejects unjoined home records, active remote records and unsent
  messages, active barriers and staged writes. Applications must coordinate a cluster-wide quiescent phase before
  calling it; finalize is not a distributed shutdown barrier. Finalize LoomPar
  before LoomMem. Runtime destruction is not a substitute for distributed join.
  Reinitializing a finalized multi-host runtime is not supported by the existing
  LoomMem poller ownership model.

## Validation

`cmake -S . -B build && cmake --build build -j 8`

`ctest --test-dir build --output-on-failure`

- Local test: execution on a distinct native thread, blocked join before completion,
  concurrent creators, function errors, foreign IDs, repeated join and reclamation.
- Acceptance scale is **16 containers** (`cxloom-h0` through `cxloom-h15`).
  The example rejects any host count other than 16; a two-container smoke test
  is no longer an acceptance target.
- The process regression uses 16 independent processes and a fresh file-backed
  mapping. Each host has four concurrent creators. Each creator launches a parent
  on each of the other 15 hosts; each parent launches and joins a child on its
  next host. A capacity-2 queue exercises backpressure. Every round verifies
  exact execution counts and zero home records before advancing. This covers
  all 240 directed host pairs, copied arguments, nested create/join, completion,
  reclamation and a missing remote function. Three regression rounds execute
  5,760 native invocations across the cluster.
- Container entry point: `scripts/run-loompar-containers.sh`. It checks that all
  16 containers are running before starting host 0, builds in every container,
  and requires a PASS record and successful exit from every host. Existing
  containers must expose `/workspace` and the same `CL_DAX_DEVICE`. Run exclusively:
  host 0 reformats the shared region.
- Defaults: `CL_PAR_ROUNDS=300`, `CL_PAR_CREATORS=4`,
  `CL_PAR_ROUND_DELAY_MS=1000`, `CL_PAR_TIMEOUT_SECONDS=1800`. This executes
  576,000 native invocations over at least five minutes, with progress reported
  every ten rounds. Rounds, creator concurrency, inter-round delay and timeout
  can be overridden without changing the required 16-host scale.
- A longer process-only soak can be run with:

  ```bash
  CL_PAR_TEST_ROUNDS=60 CL_PAR_TEST_DELAY_MS=5000 CL_PAR_TEST_TIMEOUT_SECONDS=900 \
    python3 tests/loompar_process_test.py build/cxloom_loompar_threads
  ```

  This runs 115,200 native invocations over at least five minutes. It checks
  reclamation each round but does not measure RSS or establish absence of all
  leaks. Phase synchronization uses existing bootstrap probes as a test harness;
  it is not an implementation or validation of LoomPar barrier semantics.
- The 16-container script was attempted in this session and stopped during Docker
  preflight: access to `/var/run/docker.sock` is denied by the environment.
  Container/devdax acceptance remains **pending**. File-backed process tests
  cannot establish container isolation or non-coherent physical CXL visibility.

## Recorded 16-host results (2026-09-05)

- Full CTest regression: 14/14 passed; the 16-process test completed in 6.31 s.
- Process soak: all 16 hosts passed 60 rounds with four concurrent creators per
  host and a 5 s inter-round delay. Each host executed 7,200 native invocations,
  totaling 115,200. Every round checked counts and zero home records; all hosts
  finalized successfully. Elapsed time ranged from 407.218 to 407.219 s.
- Full soak output: `/tmp/cxloom-par-16-long.log` (session-local evidence).
- Actual 16-container run: blocked at Docker preflight by socket access denial;
  no container workload was started and container acceptance is not complete.

## Next steps and exclusions

Synchronization is implemented in the next milestone: see
[LoomPar synchronization](loompar-synchronization.md) for the fixed-world blocking
barrier, generation protocol, staged-write ownership and create/join boundary hooks.
The current regression now has 16 passing tests, including a separate 16-process
synchronization workload. The 14-test and soak results above record the earlier
execution-only baseline, not a fresh soak of the synchronization implementation.

Load tracking and per-host admission limits are now implemented; see the
scheduling section in [the synchronization contract](loompar-synchronization.md).
The next step is richer cross-node/container scheduling and, if required,
execution-state transfer for running-thread migration. The outbound control
queue remains bounded only by available host memory.

The future scheduling extension needs an explicit execution-state transfer model
if it includes running-thread migration; the current milestone moves launch
metadata only. Host crashes and transparent recovery are excluded. The transport
assumes reliable delivery without retransmitted CREATE requests; duplicate launch
suppression and recovery protocols are not implemented. LoomMem non-coherent
multi-host validation remains deferred and does not block these execution stages.
