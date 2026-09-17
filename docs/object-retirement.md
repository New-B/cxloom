# Object Retirement and Safe Space Reuse

> The implemented read path is specified in [Read Access under Release Consistency](read-access-design.md).

LoomMem uses object-level lifetime management and block-level data coherence.
An object has an address, extent information, and lifecycle state. It has no
per-allocation identity or generation. Block versions and token epochs remain
local to a live block and may restart when storage is reused.

Applications must stop issuing accesses before freeing an object. Using an old
GPtr, request handle, lease, or write buffer after its lifetime is a programming
error. The runtime nevertheless drains operations it already accepted, its
caches, deferred publication, and asynchronous protocol activity before storage
becomes available to a new allocation.

## Invariant

Neither data extents nor block-metadata extents may enter a free pool until:

1. every configured host has stopped producing activity for the object;
2. all admitted reads, cache fills, write leases, and staged writes have ended;
3. all previously published protocol packets and their handlers have finished;
4. every host has removed the object's reusable cache and token state.

This applies to exact reuse, partial overlap, splitting an old extent, and
coalescing several old extents. A new allocation need not have the same shape.

## Stable admission and metadata lifetime

The allocator header exists for the entire shared-region session. Its global
lock protects allocation, retirement control, and final extent return. Existing
object accesses use stable allocation lookup slots followed by the object's
descriptor lock; they never take the allocator lock. A slot pin prevents
reclamation while looking up or waiting for the descriptor lock. Metadata-only
queries copy under this protection; raw descriptor results require an active
operation or ownership of the retirement phase before dereferencing.

Replica references count hosts holding cached blocks across both indexes. They
do not count read calls. Separately, `active_operations[host]` protects admitted
CXL copies, token metadata operations, and write-buffer lifetimes. Current-index
hits read immutable DRAM without admission or lifecycle checks. Old-index hits
and misses require admission and are rejected once the object is RETIRING.

An admitted CXL read may finish during retirement. Writers may commit or abort;
staged writes remain pinned until their originating thread runs
SynchronizeRelease. A per-block release during retirement cannot grant the
token to another waiter. Closing acknowledgement seals the host under the
object lock after checking its active operations are zero.

## Shared retirement control

The allocator header contains one region-wide retirement transaction. Its
sequence number identifies only this control transaction; it is not attached to
allocations, normal token messages, replicas, or application handles. The
sequence never wraps: exhaustion fails explicitly.

The control slot records the object address, coordinator, fixed participant
count, phase, per-phase host acknowledgement masks, and a producer-to-consumer
queue-watermark matrix. Access to this slot uses the header lock, so a stale
phase observation cannot acknowledge another phase or transaction. There are
no retirement notification/acknowledgement packets to remain in flight after
reuse. Every runtime polls the shared slot as part of its progress loop.

Only the allocation owner can begin or resume retirement. Reclamations are
serialized through this slot; other objects may still be allocated and read
or written. Concurrent frees on different hosts wait for the slot subject to
their deadline. Concurrent frees on one host may return Unavailable and require
retry. A timed-out transaction retains the slot until its owner finishes it.

## RETIRING phases

The allocation remains RETIRING throughout all phases below. The phases belong
to the shared control transaction, not to object data consistency.

### Closing

The coordinator changes ALLOCATED to RETIRING under the object lock. New CXL
admissions and token requests fail; valid current-index DRAM hits may continue. Each host's progress callback then:

- serializes with local token producers and message handlers;
- wakes and removes the object's pending waiters;
- releases granted but unclaimed leases without publishing modifications;
- clears owner-side pending requests;
- discards unsent packets for this object from its deferred outbound queues;
- waits for claimed leases and admitted references to reach zero;
- records each outbound queue's published sequence, then acknowledges closing.

Incoming token packets for a RETIRING object are consumed without forwarding,
granting, replying, or installing data. Every participant cancels its own
waiters, so no remote cancellation reply is required for retirement. Requests
that race the transition may finish their existing handler before local closure;
any packets they produced are included in the sender's recorded watermark.

Once a host acknowledges closure it cannot acquire a retirement-only reference
to the object either. This prevents a late internal operation from reopening
activity after that host has certified quiescence.

Token sends use a locally serialized, per-destination deferred FIFO. A full
shared queue never makes the poller wait for another poller: progress attempts
nonblocking flushes, and retirement discards unsent packets for its object.
The shared SPSC producer lock serializes publication with other runtime traffic.

### Draining

Only after all configured hosts acknowledge closing does draining begin.
For each producer, the receiving host waits until it has processed through that
producer's recorded watermark on the corresponding inbound channel.

The progress callback samples the consumer cursor only after ScanOnce has
returned from all popped-message handlers. The queue advances its head before
calling the handler, so sampling the head from the freeing thread would be
incorrect. In a multi-host runtime only the poller performs retirement progress.
A single-host runtime has no channels and may progress synchronously in FreeShared.

After all closing acknowledgements, no host can produce additional packets for
this object. Retiring-object handlers generate no descendants. Consequently,
one cut across all directed channels suffices; cross-channel arrival order does
not matter. Other objects' traffic may continue beyond the fixed watermarks.

### Cleaning

After all hosts acknowledge draining, each host removes the object's block
replicas from both indexes, clears its replica-holder bit, and removes its
per-block token/arbitration state.
No admitted reader or writer can reinstall a replica at this point. The final
acknowledgement is published only after cleanup completes.

ReadSnapshot/ReadView values already returned to applications may retain
independent immutable DRAM storage. They no longer participate in cache lookup
or reference CXL storage, and cannot become replicas of a new object. Physical
freeing of those private copies follows their normal shared ownership lifetime.

### Reclaimable

After all cleaning acknowledgements, the coordinator exclusively closes the
stable allocation slot and verifies zero active operations and replica holders.
It then invalidates the descriptor and returns both data and sidecar extents
under the allocator lock. A lookup still pinning the slot postpones reclamation;
the coordinator retries until its deadline. The slot becomes idle before releasing the lock.
Allocation cannot observe either returned extent before descriptor invalidation
and transaction completion. Calling the allocator's reclaim primitive without
the matching completed transaction fails.

## Timeout, failure, and shutdown

FreeShared returning success means reclamation is complete. A timeout after
retirement begins returns Unavailable while leaving the object RETIRING and
its storage reserved. Retrying on the owner resumes the same transaction and
sequence. A timeout while waiting to obtain the global slot does not change
the requested object's lifecycle state.

There is no transition back to ALLOCATED. Missing hosts, stopped pollers, active
write buffers, or unreleased references prevent completion; timeout never
serves as permission to reuse memory. A failed participant cannot be removed
from the acknowledgement set without a separate fencing/recovery protocol.
Host restart and membership changes are outside this implementation.

All configured hosts must keep progress alive until reclamation finishes.
Runtime finalization rejects an active shared retirement transaction. An
application must not wait for a synchronous free from inside the queue callback
whose completion that free needs; such an attempt times out rather than freeing
storage early.

## Costs and validation

This initial protocol deliberately trades reclamation throughput for a simple
completion proof: one concurrent retirement, O(H^2) watermark storage in the
region header, and O(H) progress work per participant. The existing shared
allocator lock is limited to allocation and retirement control. Ordinary
admission uses object locks and stable discovery slots; allocator sharding and
batched retirement are
separate future optimizations that must preserve the invariant above.

Bootstrap layout 12 and allocator layout 12 reject older shared regions. All
hosts must use the same rebuilt protocol and reinitialize their shared region.

The retirement test covers all-host cache eviction, a paused message callback,
a saturated token queue plus deferred sends, pending-request cancellation,
timeout/retry without resurrection, protection from premature space reuse,
active and staged writers, immutable detached snapshots, partial-overlap reuse
with equal block versions, and coalesced larger allocations. Existing block,
coherence, allocator, and multi-process tests exercise the same protocol.
