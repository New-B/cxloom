# Queue-Based Write Token Protocol

LoomMem serializes mutations of each shared allocation with one write token.
The allocator initializes the token to the allocating host with version 0 and
epoch 1. AllocationDescriptor token_owner, version, and token_epoch are the
authoritative CXL-resident state; queue messages are notifications, not an
alternative source of truth.

A host calls RequestWriteToken, keeps the returned request handle, and waits
with WaitForWriteToken. If the token is remote, TOKEN_REQ is sent on the
requester's outbound SPSC queue. A stale owner forwards the request using its
own outbound queue while preserving the original requester and request ID.
The current owner serializes accepted requests per object in local FIFO order.

ReleaseWriteToken performs the handoff in this order:

1. validate generation, owner, epoch, and the locally active lease;
2. publish the mutated object using the configured visibility recipe;
3. increment and publish the object version;
4. increment the token epoch and publish the new owner;
5. enqueue TOKEN_GRANT to the new owner.

The receiver acquires the shared descriptor and accepts a grant only when its
generation, owner, version, and epoch match the grant. A lease from an earlier
epoch cannot be released again.

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
the same handle again. Automatic retry and host-failure recovery are not part
of this first protocol version.

## Runtime integration

StartQueuePoller always installs an internal dispatcher for TOKEN_REQ and
TOKEN_GRANT; an optional application handler receives all other message kinds.
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
