# Read Access under Release Consistency

Status: implemented. This document specifies read admission, replica validity,
and replica-reference accounting shared by the coherence and retirement protocols.
Bootstrap and allocator layout versions are both 12; existing shared regions
must be reinitialized and all participating hosts rebuilt.

## Locks and object lifetime

The shared global allocator lock is used only for allocation and freeing,
including retirement control. Existing-object operations use a shared lock in
the object's allocation descriptor to serialize admission and lifecycle
changes. Replica-holder bits are updated atomically while local cache bookkeeping
is locked; an admitted operation protects first installation, and existing
membership protects removal. Local cache operations use host-local synchronization. Reads served entirely from valid local DRAM need neither
shared lock nor shared metadata access. Different objects do not share an
access lock. Block tokens continue to serialize conflicting writers; the
object lock must not be held while waiting for a token or remote progress.

Region-lifetime allocation discovery slots protect the descriptor lock itself.
A lookup first pins its stable slot, verifies the address again, then takes the
descriptor lock to register an active operation. Reclamation exclusively closes
the slot before touching descriptor storage. Slot pins count concurrent lookups;
they neither serialize different objects nor take the global allocator lock.
The fixed allocator header contains 2,048 slots, limiting simultaneous live or
retiring allocations to that count; exhaustion returns Unavailable without
reserving extents. Slots are reusable after reclamation. They are internal
lifetime guards and do not add allocation generations to GPtrs.

The lock order is allocation/free lock (only on allocation/retirement paths),
stable slot pin when needed, then descriptor lock. Admission releases the
object lock and slot pin after registering `active_operations[host]`. Retirement
seals each host under the object lock only after its operations reach zero;
sealed hosts cannot register new retirement-only operations.

## Replica references count hosts

For one object, the shared replica reference count equals the number of hosts
holding at least one reusable cached block of that object. A host has one
membership across both of its replica indexes, regardless of the number of
blocks, views, threads, or reads.

- Installing a host's first cached block adds one membership.
- Filling another block, rereading a block, or moving entries between indexes
  does not change membership.
- Removing the host's last cached block removes one membership.
- Replacing stale blocks while retaining host membership does not temporarily
  add another reference. If all copies are actually dropped and admission is
  released, a later fill must register afresh.

A local per-object record tracks the union of blocks in both indexes and
serializes concurrent first installs and last removals. Cache capacity applies
to both indexes together. Eviction, explicit invalidation, retirement cleanup,
and index clearing all use the same membership bookkeeping.

Replica membership is not an operation counter. In-flight CXL copies, metadata
operations, write leases, and staged publication still require independent
lifetime protection until they complete. An immutable ReadView already returned
to the application may outlive cache eviction as detached DRAM storage; its
shared_ptr ownership is not a CXL replica reference.

## Two indexes and synchronization boundaries

Each host maintains `current` and `old` replica indexes. Each entry records
object dimensions, cached block coverage, and per-block versions. Cached
versions retain their existing meaning; there is no object-wide data version.

At a synchronization boundary, serialize with local accesses, discard the
previous `old` index using normal membership bookkeeping, move `current` to
`old`, and make the cleared index the new empty `current`. Rotation invalidates
the assumption of freshness; it does not eagerly fetch every cached block.
Release-side publication and acquire-side visibility ordering must complete in
the order required by the synchronization operation before post-boundary
validation. `SynchronizeAcquire` performs the rotation for thread start, join,
and barrier acquire hooks, including host-local threads sharing these indexes.
`SynchronizeRelease` publishes staged writes before the release fence; the
acquire side of synchronization rotates indexes.

A host-local shared boundary lock covers reads/fills and local publication;
rotation takes it exclusively, so no pre-boundary fill can install into the new
current index. Reads/fills of the same object are locally serialized, while
unrelated objects can fill concurrently. The directory/LRU mutex is held only
for local bookkeeping, not while waiting for shared metadata or copying CXL.

## Read procedure

1. Look up the object and requested blocks in `current`. Covered blocks are
   valid for this synchronization interval: serve them directly without reading
   CXL versions, lifecycle state, or replica-reference metadata.
2. For requested blocks absent from `current`, inspect `old`. Before using old
   blocks, enter protected CXL admission and check authoritative object state.
   Reject admission for RETIRING objects. For an allocated object, validate the
   old blocks against the authoritative block versions using the existing
   stable epoch/version observation rules.
3. Move matching old blocks into `current` and reuse their DRAM bytes. Discard
   mismatching old blocks and fetch their latest committed CXL contents using
   the stable-copy protocol. Insert successful copies into `current`.
4. If neither index covers a requested block, use protected admission and fill
   that block from CXL according to the cache policy. Already holding other
   blocks of this object does not add a host reference.
5. Assemble the requested ReadView from the valid blocks. Failure to admit or
   fill any required block fails the requested range; partial coverage does
   not constitute a complete successful read.

Promotion is per validated block: validating one block must not silently mark
other cached blocks of the object fresh. Unrequested old blocks may stay in
`old` until accessed or cleared at the next boundary. Independently validated
blocks do not promise an atomic multi-block snapshot. Current blocks and newly
filled blocks may have different committed versions, as permitted by the
release-consistency model.

CXL copying retains the existing protocol: observe an even writeback epoch and
version, copy bytes, observe metadata again, and accept only if both values are
unchanged and the epoch is even. Token epochs and content-version design are
unchanged. Local committed writes must update or invalidate affected cached
blocks so subsequent local reads observe the host's own published writes.

## Retirement and address reuse

A fully covered `current` hit can serve local DRAM even if retirement has
started remotely. It need not query whether the CXL object is still live.
Explicit invalidation removes affected cache entries; subsequent misses must
consult CXL. Old entries also require admission because they are no longer
valid without authoritative validation.

RETIRING rejects new CXL admissions, fills, and token requests, not reads
already satisfied by valid local DRAM. Retirement must serialize local cleanup
with lookup/promotion/fill so no reusable entry can be reinstalled after cleanup
acknowledgement. It drains accepted CXL activity separately, clears both replica
indexes and holder membership, and only then permits storage reuse. Returned
immutable views can retain detached private storage.

A GPtr is an address, not an allocation identity. After reuse, an access without
a usable cache resolves the allocation currently at that address and checks its
bounds; it may therefore access the new object. The runtime does not detect
expired GPtrs. Applications own pointer lifetime correctness. Block versions
and epochs must not be presented as protection against address reuse.

## Implementation validation

Required behavior includes no shared metadata access on current hits;
independent-object admission; one host membership under concurrent first fills;
unchanged membership during partial-cache extension and promotion; last-block
removal across both indexes; refresh after a boundary; per-block promotion;
repeated boundary clearing; rotation racing fills; explicit invalidation;
retirement racing local hits and CXL misses; detached views surviving cleanup;
and new-allocation resolution after address reuse without a generation check.

## Application API

`WriteView::Stage()` transfers an edited buffer to the runtime and makes the
view inactive. `clSynchronizeRelease(context)` publishes the calling native
thread's staged writes in staging order and executes release ordering. Staging
retains write tokens and operation pins until that release; other threads do
not publish those buffers. Applications must stop using mutable pointers after
staging and release before the staging thread exits or the context is destroyed. After the application observes its synchronization event,
`clSynchronizeAcquire(context)` rotates the two indexes with acquire ordering.
These calls do not themselves implement a cross-host rendezvous; LoomPar hooks
invoke the runtime operations at its existing synchronization boundaries.
`clInvalidate(context, object)` explicitly clears that host's cached blocks in
both indexes, preserving already returned immutable ReadViews. Cache count and
byte diagnostics report both indexes combined.
