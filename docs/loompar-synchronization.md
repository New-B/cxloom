# LoomPar synchronization contract

## Participant and generation model

Applications see this mechanism through `cl_pthread_barrier_init` and
`cl_pthread_barrier_wait`; host IDs, queue messages and generations are runtime
state. The C++ barrier manager described below is an implementation boundary.

`LoomParRuntime::Barrier(id, local_participants)` is a blocking world collective.
Every configured host participates. The runtime supports the configured host count;
all multi-host regression and container acceptance runs use 16 hosts.

Each host supplies a positive local participant count, fixed for that barrier ID
for the runtime's lifetime. Counts may differ across hosts (the integration test
uses 1 on even hosts and 2 on odd hosts). A participant is a calling native thread or LoomPar fiber
on the execution host, regardless of its GTID's home. Application main threads and
LoomPar workers can participate. Each member of the application-defined cohort
calls once per round. GTIDs are not used as barrier membership identities. Counts
are enforced, but replacement of a cohort member by another thread is not detected.
There are no host subsets, zero-participant hosts, dynamic membership or implicit
world participation by all existing threads.

Generations begin at zero and advance only on global completion. Local calls
aggregate under a mutex; the last local participant enqueues one BARRIER_ARRIVE
for coordinator host 0. The coordinator tracks one bit per host, including its
own local cohort. Only a full host bitmap produces BARRIER_RELEASE for that
barrier ID and generation. Each host notifies its local waiter queue. Native waiters sleep on a condition
variable; fibers park and release their worker until notification. Neither native
waiters nor the poller spin waiting for barrier completion.
Outbound messages use the existing progress queue, keeping the inbound poller free
to handle arrivals, releases, creates, completions and token traffic.

Remote arrivals can precede the coordinator's first local call. Duplicate arrivals
count once; old arrivals/releases are ignored. Future generations or invalid
release ordering break the affected barrier. A conflicting local participant
count or a release-hook failure also breaks it and broadcasts failure to the
other hosts. The failure is sticky for that ID: waiting and future calls fail,
while other IDs continue to work. Zero participant counts are rejected before
participation. Missing participants wait indefinitely, as with a conventional
blocking barrier; this is not a timeout, crash-detection or recovery protocol.

Finalize rejects active API waiters or unfinished barrier rounds. A completed
barrier is not a distributed shutdown protocol: applications must still ensure
all task, token and retirement traffic is drained before shutting down pollers.

## LoomMem boundary hooks

| Boundary | Action and ordering |
| --- | --- |
| Before successful create dispatch | Creator calls SynchronizeRelease before allocating/launching the home record or enqueuing CREATE_REQ. Invalid name/placement/argument requests do not publish. |
| Before worker function entry | The execution thread calls SynchronizeAcquire before invoking the registered function, locally and remotely. |
| Before completion | The worker calls SynchronizeRelease before MarkCompleted; remote COMPLETE_NOTIFY follows fiber execution reclamation. |
| Before join returns after completion | The joining thread calls SynchronizeAcquire after observing completion, including failed execution, and then removes the home record. Invalid joins do not acquire. |
| Barrier arrival/return | Every participant calls SynchronizeRelease before contributing an arrival and SynchronizeAcquire after the matching global release. |

The local barrier mutex folds all local participants' publications into the last
arrival; queue publication, coordinator aggregation, release delivery and local
condition-variable notification carry that ordering to the acquiring participants.
The same model orders create and completion traffic. These hooks currently use
the existing release/acquire visibility backend, not a newly validated physical
non-coherent cache-flush recipe.

## Write ownership and immutable reads

Existing `ReleaseWriteBuffer` remains an immediate commit operation. Merely
acquiring a mutable buffer does not opt it into automatic publication. To defer
commit to the next synchronization boundary, finish editing it and transfer it:

```cpp
auto write = mem.AcquireWriteRange(object, offset, bytes, timeout_ms);
// Check write.status(), then fill write.value().data().
auto staged = mem.StageWriteBuffer(&write.value());
// Check staged. On success, write.value() is empty and must not be used.
auto child = par.CreateThread("registered-worker", args, placement);
// The successful create publishes this creator's staged writes before dispatch.
```

Applications using the public LoomMem API can transfer a `WriteView` with
`Stage()` and publish at an explicit release boundary on the same context:

```cpp
auto write = cxloom::clWriteRange(context, object, offset, bytes);
if (!write.ok()) return write.status();
// Fill write.value().data(); finish all edits before transferring ownership.
auto status = write.value().Stage();
if (!status.ok()) return status;
// The view is now inactive. More disjoint writes can be staged here.
status = cxloom::clSynchronizeRelease(context);
if (!status.ok()) return status;
// Now publish the application's synchronization event to readers.
```

`Stage()` uses the same runtime staging mechanism as `StageWriteBuffer` below.
It binds the write to the calling execution context (fiber or native thread), even if a different context
originally acquired the view. The ownership transfer between threads must be
synchronized by the application. Successful staging disables `data`, `Commit`,
`Abort`, and repeated `Stage` calls on that view; its destructor no longer aborts
the buffer. On failure an active view remains owned by the application.
`clDestroy` rejects a context containing staged writes and retains it for retry.
The staging execution context must release before exiting; another context's
release does not drain its pending writes. No acquire boundary publishes staged writes.

StageWriteBuffer requires exclusive ownership of the buffer's storage and object
reference. It empties the source buffer and stores it under the staging
execution context's ID. Do not retain raw mutable aliases, make new aliases, mutate a staged
buffer, or try to release it again. A staging thread can differ from the thread
that acquired the buffer only through a properly synchronized ownership transfer.
Staging keeps tokens held until the next release: reacquiring the same held block
before releasing it is invalid application usage and can wait for its own token.

SynchronizeRelease commits only the calling execution context's staged buffers, in staging
order, through ReleaseWriteBuffer. Other contexts' staged buffers are unaffected, including other fibers on the same worker.
Ordinary explicitly released writes already have their data/version publication;
the hook supplies the ordering fence even when there are no staged buffers.
On a commit failure the boundary returns failure and attempts to abort the current
and remaining staged buffers. Already published buffers are not rolled back; this
is not a multi-object atomic transaction. LoomPar worker completion attempts this
release even if the function throws, reports failure through join, and runs the
join acquire hook. A failed worker-start acquire skips the function.

SynchronizeAcquire executes the acquire fence and rotates the host's two
replica indexes: the previous old index is cleared, current becomes old, and
current starts empty. Subsequent reads validate only requested old blocks,
promoting unchanged replicas or fetching changed/missing blocks from CXL.
Current hits use local DRAM without shared metadata access. Both indexes share
one host membership per object and the same bounded LRU capacity. Snapshots already returned to
applications remain immutable and keep their old values; reacquire after a
synchronization boundary to see subsequent writes. No hook makes arbitrary direct
mapped writes, private pointers or data races valid shared-memory accesses.

LoomMem and LoomPar Finalize reject staged writes. Application-created native
threads must explicitly release, use a barrier, or otherwise call
SynchronizeRelease before exiting; only LoomPar-managed workers get the automatic
completion hook. These are C++ runtime APIs; no new C ABI is introduced here.

## Validation and current limits

- Full regression: 16/16 tests passed after integration.
- Local tests cover blocking across generations, count mismatch and sticky failure,
  independent IDs, 16-host duplicate/early arrivals, finalize protection, hook
  order and acquire failure skipping the function while still cleaning up/joining.
- The 16-process shared-file test covers local and remote create/start/completion/
  join publication, exception completion, immutable old snapshots, and 24 rounds
  of staged writes by 24 participants across 16 hosts. A second barrier per round
  prevents the next writes from racing the readers. A delayed participant exercises
  blocking. A mismatched count on host 0 propagates failure to all 16 hosts, after
  which an independent barrier succeeds. Objects are retired before shutdown.
- Run `ctest --test-dir build --output-on-failure` for the regression.
- For container acceptance, rebuild/start the existing 16-host environment and run:

  ```bash
  CL_PAR_PROGRAM=cxloom_loompar_sync ./scripts/run-loompar-containers.sh
  ```

  This fixed synchronization workload runs 24 rounds, independently of the
  execution-soak CL_PAR_ROUNDS settings. All 16 hosts must print PASS. The script
  initializes the configured shared region and therefore requires exclusive use.
- The current file-backed 16-host process/container harness validates protocol
  behavior. Physical non-coherent DAX/CXL visibility and performance remain
  environment-dependent. Host crashes and transparent recovery remain outside
  this version.

## Scheduling and resource control

The runtime publishes a compact versioned `LOAD_UPDATE` to every peer on
create/completion events and periodically. Samples contain executing, ready,
blocked and worker counts in addition to pending creates and queue depth.
Automatic placement filters hosts at configured limits. The configured policy is
memory-aware by default, or can be round-robin or least-loaded.

`max_running_threads_per_host` and `max_pending_creates_per_host` are independent
per-execution-host limits; zero means unlimited. Local creates fail before a
request is emitted when the pending limit is reached. Remote hosts recheck their
running limit on CREATE_REQ and return a failed completion when admission is
denied. Snapshots are advisory and can be stale, so the target-side check is
mandatory. Host failure handling, cancellation and dynamic barrier membership
are not provided. Running capacity is released once execution completes, before completion becomes
observable to join or detach. Completed-but-unjoined home records retain their
result but no running slot; detached records are reclaimed automatically.

The memory-aware scheduler score is:

```text
score = aged_running + aged_pending
      + queue_weight * queued_messages
      + history_weight * outstanding_launch_history
      + locality_penalty
```

Remote samples carry a monotonic sequence and a receiver-local timestamp.
Detailed samples estimate CPU demand as `(executing + ready) / workers`;
blocked Fibers remain active for admission but do not add CPU demand. A
least-loaded policy uses that estimate and predicts launches not yet visible in
remote telemetry. Round-robin advances over eligible hosts. The memory-aware
policy adds queue, launch-history and current-version coherence terms. Working
set locality is queried only by the memory-aware policy. Explicit placement
bypasses scoring but remains subject to target admission.

The current thread model is pinned: after CREATE_REQ is admitted, its fiber
stack and invocation remain on that execution host until completion. A future
migration protocol therefore requires an application safe point, serialized
thread state, home-host ownership transfer, destination admission, and a
migration epoch to reject stale completion messages. No such protocol is enabled
by this milestone; running threads and their stacks are never implicitly moved.
