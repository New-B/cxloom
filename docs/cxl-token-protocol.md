# Queue-Based Write Token Protocol

LoomMem serializes mutations independently for each coherence block. The
allocator initializes every block sidecar to the allocating host with version
0 and token epoch 1. Sidecar metadata is authoritative; queue messages are
notifications rather than an alternative source of truth.

A host calls RequestWriteToken, keeps the returned request handle, and waits
with WaitForWriteToken. If the token is remote, TOKEN_REQ is sent on the
requester's outbound SPSC queue. A stale owner forwards the request using its
own outbound queue while preserving the original requester and request ID.
The current owner serializes accepted requests per block in local FIFO order.

ReleaseWriteToken performs the handoff in this order:

1. validate allocation ID, block identity, owner, epoch, and active lease;
2. publish the mutated block using the configured visibility recipe;
3. increment and publish the block version;
4. increment the token epoch and publish the new owner;
5. enqueue TOKEN_GRANT to the new owner.

The receiver accepts a grant only when its allocation ID, block owner, version,
and epoch match authoritative metadata. A delayed message for a freed object
cannot affect a later allocation at the same GPtr because every allocation
receives a new monotonic allocation ID.

## Cross-queue ordering

Round-robin polling deliberately does not impose arrival-time order across
different producers. Correctness therefore does not depend on such an order.
The current owner establishes the arbitration order when it drains requests
and appends them to the per-object pending queue.

There is also an explicit transfer window: metadata can name a new owner before
that host drains TOKEN_GRANT. The new owner's local token state remains
unavailable until the grant arrives. Requests drained from other inbound queues
during this window are queued and cannot trigger a second grant. After the
granted writer releases, those requests are served in the established local
order.

The request handle remains active after a wait timeout, so callers may wait on
the same handle again or cancel it. Owners explicitly answer an invalid,
stale-allocation, or retiring request with TOKEN_REJECT. Cancellation uses
TOKEN_CANCEL and TOKEN_CANCEL_ACK; if a grant won the race, the requester
immediately releases the late grant. Both synchronous cancellation and the
fire-and-forget cancellation path reclaim terminal waiter state. Automatic
retry and host-failure recovery are not part of this protocol version.

## Retirement drain

After the allocation owner changes an object from ALLOCATED to RETIRING, it
sends TOKEN_RETIRE for every coherence block to that block's current token
owner. The token owner atomically detaches its local pending queue, completes
every detached request with TOKEN_REJECT(kRetiring), and then replies with
TOKEN_RETIRE_ACK. Stale owners forward TOKEN_RETIRE according to authoritative
sidecar ownership.

FreeShared does not begin reference quiescence or release the descriptor and
sidecar extents until every block acknowledgment arrives. Requests that race
with the drain observe RETIRING in HandleRequest and are rejected directly.
Consequently, deleting per-allocation token state cannot strand a normally
queued requester.

## Runtime integration

StartQueuePoller always installs an internal dispatcher for TOKEN_REQ,
TOKEN_GRANT, TOKEN_REJECT, TOKEN_CANCEL, TOKEN_CANCEL_ACK, TOKEN_RETIRE, and
TOKEN_RETIRE_ACK; an optional application handler receives all other message
kinds.
Token APIs require a shared runtime and a running poller.

cxloom_token_test maps two runtimes to the same backing file and covers a
blocked remote request, both handoff directions, data visibility, monotonic
version/epoch changes, wait timeout reuse, and stale-lease rejection.

## Variable-scale stress results

The CXL/devdax stress test runs 2 to 64 NUMA-pinned containers contending for one
shared object. Each acquired lease verifies the previous checksum, mutates the
object, and releases the token. The final owner checks the exact counter and
version against `host_count * iterations`.

During initial testing this exposed an orphaned-request bug: after granting the
first pending request, the old owner retained the rest of its local pending
queue even though ownership had moved. The handoff now forwards the remaining
requests to the new owner. Per-destination outbound mutexes also preserve the
single-producer property when application and poller threads send concurrently.

Pass the scale once when launching containers. Later test scripts discover the
persisted `CL_HOST_COUNT` from container zero when neither a positional argument
nor an environment override is supplied:

```bash
./scripts/launch-numa-containers.sh 12
./scripts/run-token-stress-containers.sh
```
