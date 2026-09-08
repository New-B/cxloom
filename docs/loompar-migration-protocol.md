# LoomPar migration protocol design

The public `cl_pthread_*` API exposes only a cooperative checkpoint,
`cl_pthread_migration_safe_point(runtime)`. The callback must call it at a point
where its state is represented by LoomPar metadata and GPtrs. The current
runtime keeps a thread pinned after create; the checkpoint increments its
epoch and yields the Fiber context. Completion messages carry the current
epoch, and stale notifications are discarded by the home runtime.

1. The home host increments a migration epoch and sends `MIGRATE_REQ` only when
   the execution record is at an application safe point. A running callback must
   have cooperated with that safe point; arbitrary native stacks cannot be
   captured safely by LoomPar.
2. The execution host stops at the safe point, publishes staged LoomMem writes,
   serializes the registered function ID, argument state and migration epoch,
   and acknowledges quiescence. It retains the old record until commit.
3. The destination performs normal resource admission, reconstructs the local
   execution record and acknowledges readiness. It resolves the function ID from
   the same cluster function manifest used by CREATE_REQ.
4. The home host commits ownership to the destination, sends the resume message,
   and marks the previous execution epoch obsolete. Completion notifications
   carrying an older epoch are ignored.
5. At most one epoch may be running. If destination admission fails, the old
   execution record resumes. A failure after ownership commit requires recovery
   and is intentionally outside the current version.

`MigrationRequest` and `MigrationAck` are reserved internal message formats in
`common/messages.h`; no handler currently accepts them. Implementing handlers
requires a safe-point API and a serializable invocation context. `std::thread`
stacks, arbitrary callback locals, process-local pointers and unregistered
function closures are not migratable. Host crashes and transparent recovery are
also excluded. Request/ack handlers remain disabled until a resumable execution
context is available.

## Executable home transaction model (2026-09-06)

`include/cxloom/loompar/migration.h` now provides `MigrationTransaction`.
It is an independently tested protocol model, not connected to the runtime's
message dispatcher. One instance belongs to one GTID at its home host; the
caller must serialize access. It tracks a monotonically increasing transaction
epoch separately from the committed execution epoch. Aborting a transaction
never changes the source's valid execution epoch.

| Event | Required state | Result / permitted action |
| --- | --- | --- |
| Begin(target) | Idle or Aborted | Requested; allocate fresh transaction epoch |
| Quiesced(source, epoch) | Requested | Quiesced; source cannot execute |
| Prepared(target, epoch) | Quiesced | Prepared; destination reserves resources but cannot execute |
| Commit(epoch) | Prepared | Committed; sole execution authority changes to destination |
| Resumed(target, epoch) | Committed | Idle; another migration may begin |
| Abort(epoch) | Requested, Quiesced, Prepared | Aborted; source retains authority |

Repeated quiescence/preparation acknowledgements and commits are harmless in
corresponding active states. Stale epochs, wrong endpoints, premature preparation
and rollback after commit are rejected. Completion is accepted only from the
current owner with its committed execution epoch, and not while quiesced or
prepared. Completion racing a request must terminate the invocation and cancel
that transaction at the runtime layer before processing another acknowledgement.
The model does not itself store terminal invocation state.

## Required transport and continuation integration

The wire identity must include bootstrap session, GTID, transaction epoch,
source, destination, function ID, continuation ABI/version and a GPtr plus length
for serialized checkpoint state. Authenticate endpoints against the home record;
never trust a payload's claimed source. Dispatch must look up the transaction by
GTID before applying the model. A process-local `ucontext_t` is not a serializable
continuation: applications need a registered resumable step function and explicit
program counter/state in the checkpoint. Safe-point yielding alone cannot meet
this contract.

Before QUIESCED, source stops scheduling the continuation and completes its
LoomMem release hook. Retain the source record and a live reference to checkpoint
storage until terminal acknowledgement. PREPARE must validate function/ABI and
checkpoint bounds, pin the allocation and reserve destination capacity. Only
COMMIT permits destination acquire and execution; acknowledge RESUMED after
installing that authority. Source cleanup follows commit acknowledgement and
must never schedule the old context again.

On rejection or timeout before commit, home sends cancellation to destination
and waits for reservation cancellation before resuming source. Since an aborted
prepare cannot authorize execution, delayed preparation is rejected by epoch;
its reservation must still be reclaimed. Retain transaction tombstones until
all participating endpoints acknowledge cancellation or cleanup. After commit,
retry the same commit/resume decision; a timeout must not roll back to source.
Host failure after commit requires recovery and remains unsupported. This model
assumes the existing reliable transport and does not provide durable decisions.

Acceptance before enabling handlers must include three independent processes,
a source/destination distinct from home, exact-once continuation side effects,
LoomMem publication visibility, full-queue retries, duplicate/reordered/stale
messages, target admission rejection, cancellation cleanup, repeated migrations,
and join after ownership transfer. The new unit test covers home transition
invariants only; these distributed acceptance requirements remain open.
