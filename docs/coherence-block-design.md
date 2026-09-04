# Object and Coherence-Block Design

## 1. Decision

CXLoom separates allocation identity from coherence granularity:

- an **object** is the unit of allocation, addressing, ownership of lifetime,
  and reclamation;
- a **coherence block** is the unit of write-token arbitration, versioning,
  stable writeback, replica caching, and contention.

An object contains one or more fixed-size coherence blocks. The block size is
selected when the object is allocated and cannot change during that allocation
allocation lifetime. The existing whole-object API remains available as a range covering
all blocks.

This separation avoids forcing large objects through one write token without
turning every cache line into a separately allocated object.

## 2. Granularity Rules

The default block size is `CxloomConfig::coherence_granule_bytes`, currently
4 KiB. A future allocation-options API may override it per object.

A valid block size:

- is a power of two;
- is at least the platform coherence-line size, initially 64 bytes;
- is fixed for the lifetime of one allocation;
- may exceed the object size, in which case the object has one block.

For an object of `object_bytes` and block size `block_bytes`:

```text
block_count = ceil(object_bytes / block_bytes)
block(i).offset = i * block_bytes
block(i).bytes = min(block_bytes, object_bytes - block(i).offset)
```

Cache-line granularity is supported as an opt-in extreme rather than the
default because dense per-block metadata can otherwise approach the size of
the data itself.

## 3. Shared Metadata Layout

The inline allocation descriptor remains immediately before the object. It
contains immutable discovery information and object-lifetime state, but no
authoritative token state:

```cpp
struct AllocationDescriptor {
    uint64_t magic;
    atomic<uint32_t> state;
    HostId owner_host;
    uint64_t allocation_id;
    uint64_t object_offset;
    uint64_t bytes;
    uint64_t alignment;
    uint64_t allocation_id;

    uint64_t coherence_block_bytes;
    uint64_t coherence_block_count;
    uint64_t coherence_metadata_offset;
    atomic<uint64_t> object_version;
    atomic<uint64_t> range_commit_epoch;
    atomic<uint64_t> active_references[kMaxHosts];
};
```

`object_version` is a monotonic change counter incremented after every
successful block or range commit. It preserves a compact freshness signal for
whole-object compatibility APIs; correctness of a multi-block copy still uses
the participating block epochs and versions. `range_commit_epoch` is an
object-wide seqlock used by `kWholeRange` publication and is otherwise even.
An odd value means a whole-range commit is in progress; the following even
value publishes the entire commit.

`coherence_metadata_offset` addresses a dense array in the reserved coherence
region:

```cpp
struct alignas(64) CoherenceBlockDescriptor {
    atomic<uint32_t> token_owner;
    atomic<uint64_t> token_epoch;
    atomic<uint64_t> version;
    atomic<uint64_t> writeback_epoch;
};
```

`writeback_epoch` is even while the committed CXL bytes are stable and odd
only during an in-place writeback. Token ownership alone does not make the
committed block unreadable.

Dense sidecar arrays are allocated from an independent shared extent pool.
Free extents are address ordered, split for allocations, and coalesced on
release. Data extents use the same policy in a separate pool.

All offsets are region-relative; shared metadata never contains process-local
pointers.

## 4. Identity and Validation

A block operation is identified by:

```text
<object GPtr, allocation ID, block index>
```

A token lease additionally contains `token_epoch`. Every request, grant, and
release validates:

- the GPtr names a published allocation base;
- the allocation ID matches;
- `block_index < coherence_block_count`;
- the local host is the authoritative token owner;
- the lease token epoch matches the block descriptor.

The allocation ID is freshly assigned for every allocation and protects a new
object at a reused address from delayed protocol messages. It is internal
protocol identity rather than part of GPtr. The token epoch protects one block
against a stale lease after ownership transfer.

## 5. Range API

Applications express byte ranges rather than block indices:

```cpp
AcquireReadRange(object, offset, bytes, timeout, consistency);
AcquireWriteRange(object, offset, bytes, timeout, atomicity);
```

The runtime validates overflow and object bounds, then maps the byte range to
an inclusive block interval:

```text
first = offset / block_bytes
last  = (offset + bytes - 1) / block_bytes
```

Compatibility wrappers remain:

```cpp
AcquireReadSnapshot(object, timeout)
AcquireWriteBuffer(object, timeout)
```

They cover `[0, object_bytes)` and therefore retain whole-object behavior.

A returned range snapshot is contiguous and immutable in V1. It records the
object change version and the ordered vector of participating block versions;
one scalar version is insufficient to describe a multi-block snapshot.
Internally the cache is block-based; the runtime assembles the requested byte
range from stable block replicas. A later scatter/gather view may avoid this
final copy.

## 6. Block Read Protocol

The local replica key becomes:

```text
<object offset, allocation ID, block index>
```

The cached entry stores its block version. The cache retains only the newest
known replica for a block; immutable older versions survive only while held by
application snapshots.

For each required block, a reader:

1. acquires the block descriptor;
2. retries if `writeback_epoch` is odd;
3. records epoch and version;
4. returns a matching local immutable replica, if present;
5. otherwise copies the block from CXL;
6. reacquires epoch and version;
7. accepts the copy only if both are unchanged and the epoch is even.

The existing entry-and-byte bounded LRU applies to individual block replicas.
Eviction never invalidates snapshots already held by applications.

Blocks are validated independently in the normal mode. A multi-block read
therefore provides a collection of individually consistent block versions.
The API makes the stronger requirement explicit:

```cpp
enum class ReadConsistency {
    kPerBlock,
    kWholeRange,
};
```

For `kWholeRange`, the reader records every participating block epoch/version,
assembles the snapshot, and then verifies the full vector again. It accepts
only if all writeback epochs stayed even and every value is unchanged. This
establishes an interval during which the complete returned range was stable.
It also requires `range_commit_epoch` to be equal and even before and after
the copy. A change restarts the complete range assembly, so blocks from
different whole-range commits cannot be returned together.

## 7. Block Write Protocol

Writers operate on host-private buffers. For one block:

```text
request block token
copy latest committed block to private buffer
modify private buffer while writeback_epoch remains even
set writeback_epoch odd
copy changed bytes to CXL and publish them
increment block version
increment object version
set writeback_epoch even and publish metadata
release or transfer block token
```

Writers targeting different blocks of the same object may proceed concurrently.
Writers targeting the same block remain strictly serialized.

Partial-block writes are read-modify-write operations on the private full-block
replica. This prevents unrelated bytes in the same coherence block from being
lost.

## 8. Multi-Block Acquisition and Deadlock

A range writer may require multiple block tokens. All token sets are acquired
in the global order:

```text
<object offset ascending, block index ascending>
```

For a single-object range this reduces to ascending block index. If any
acquisition fails or times out, the runtime releases every token already
acquired without publishing, applies bounded backoff, and reports failure. It
must never wait while acquiring in a different order.

The first implementation acquires individual tokens. A batch range request is
a later optimization and must preserve the same ordering and lease validation.

## 9. Atomicity Modes

The range API makes atomicity explicit:

```cpp
enum class WriteAtomicity {
    kPerBlock,
    kWholeRange,
};
```

### Per-block

Each block is published and versioned independently. Readers may observe a mix
of old and new block versions. This is the default for partitioned arrays,
tables, tiles, and other data whose blocks have independent invariants.

### Whole-range

The writer holds all block tokens and publishes the range as one logical
transaction. A range-level commit sequence in the allocation descriptor is
made odd around publication and even afterward. Readers validate that sequence
before and after assembling the range. The writer increments every changed
block version and increments `object_version` once for the logical range
commit.

Whole-range mode preserves cross-block invariants but intentionally reduces
parallelism. It is opt-in and is not inferred from range size.

The initial block implementation supports per-block atomicity first. Existing
whole-object calls retain their current object-stable behavior until their
implementation is migrated deliberately.

## 10. Allocation and Publication

Creating a shared object becomes:

1. validate object size, alignment, and block size;
2. calculate `block_count` with checked arithmetic;
3. allocate a fresh dense block-descriptor extent from the coherence free pool;
4. allocate a fresh descriptor-and-data extent from the data free pool;
5. initialize every block with the allocating host as token owner, version 0,
   token epoch 1, and writeback epoch 0;
6. fill the allocation descriptor with the sidecar offset and dimensions, an
   object version of 0, and an even range commit epoch;
7. publish block metadata, then publish the allocation state last.

Failure before publication rolls both extent reservations back into their free
pools. Every successful allocation receives a new monotonic allocation ID.

Attaching hosts validate the bootstrap, allocator, and coherence-region layout
versions before resolving an allocation.

## 11. Reclamation Contract

Data and all block metadata share one object lifetime. Blocks cannot be freed
or reused independently.

Future reclamation uses these states:

```text
ALLOCATED -> RETIRING -> FREE
```

`RETIRING` rejects new reads, object references, and token requests. Existing
writers retain their references and may finish publication. Reclamation waits until:

- no block has an active write lease;
- every writeback epoch is even;
- every host's active-reference slot is zero;
- host-local cache entries for the old generation may only survive as detached
  immutable snapshots.

Only then does the descriptor cease to exist and both extents return to their
independent free pools. A future allocation is unrelated and obtains a new
allocation ID. Requests carrying the old ID are rejected.

## 12. Migration Plan

### Phase A: metadata split, behavior preserved

- format the coherence-region header and dense metadata allocator;
- move the existing object token/version/epoch into block descriptor zero;
- store block size/count/sidecar offset in `AllocationDescriptor`;
- initially configure every allocation as one coherence block;
- keep all current APIs and tests behaviorally unchanged;
- bump bootstrap and allocator layout versions.

Implementation status:

- runtime defaults and per-allocation `AllocationOptions` select object or
  fixed-block metadata layout;
- the coherence region now has a versioned header and atomic dense-sidecar
  cursor;
- object mode creates one block, while fixed-block mode records the calculated
  block count and initializes every sidecar descriptor;
- sidecar bounds lookup and publication ordering are validated;
- token and snapshot compatibility APIs now wrap block-based range operations;
  no authoritative token state remains in the allocation descriptor.

### Phase B: configurable single-block ranges

- add per-allocation block-size options;
- add range identity to token messages and leases;
- implement read/write ranges contained within one block;
- demonstrate concurrent writers on different blocks and serialization on the
  same block;
- change the local LRU key and accounting to block replicas.

Implemented: block identity is carried through requests, grants, leases, local
arbitration, versions, writeback epochs, and the bounded replica LRU.

### Phase C: multi-block ranges

- acquire tokens in canonical order;
- add rollback for partial acquisition;
- assemble immutable multi-block read snapshots;
- implement per-block publication and stress conflicting ranges.

Implemented for per-block atomicity: byte ranges map to ordered block sets,
partial acquisition rolls back, and immutable snapshots are assembled from
stable block replicas.

### Phase D: optional whole-range atomicity

- add an object/range commit sequence;
- validate range snapshots against that sequence;
- benchmark consistency strength against contention and metadata traffic.

Implemented: `WriteAtomicity::kWholeRange` changes the object-wide epoch from
even to odd before publishing any participating block and back to the next
even value only after all blocks are committed. `ReadConsistency::kWholeRange`
validates that epoch plus every participating block epoch/version. Default
range APIs retain per-block atomicity; whole-object compatibility wrappers use
whole-range consistency.

### Phase E: reclamation and adaptive policy

- retire and quiesce all blocks with their object;
- recycle data and sidecar extents after shared-reference quiescence;
- evaluate sparse metadata or coarse-to-fine adaptive splitting only after
  dense metadata costs are measured.

Object-wide reclamation is implemented with `ALLOCATED -> RETIRING`, followed
by complete descriptor invalidation. Data and dense-sidecar extents return to
separate split/coalesce pools. New allocations create fresh descriptors and
allocation IDs; stale leases and delayed messages are rejected. Adaptive
metadata policy remains future work.

## 13. Required Tests

Each migration phase must preserve the existing allocator, queue, token,
visibility, and coherence tests. New coverage includes:

- descriptor lookup and bounds for first, middle, last, and partial tail blocks;
- metadata-capacity exhaustion without a published partial allocation;
- same-block writer serialization;
- different-block writer concurrency;
- stale block-lease rejection after handoff;
- readers during private writer computation and during block writeback;
- partial-block read-modify-write preservation;
- overlapping and disjoint multi-block ranges;
- canonical-order acquisition without deadlock;
- LRU eviction and reload at block granularity;
- allocation-ID rejection after address reuse;
- devdax stress with configurable object and block sizes.

## 14. Non-Goals for the First Block Version

- transparent coherence for arbitrary raw pointers;
- automatic merging of conflicting writes to the same block;
- dynamic block-size changes during one allocation lifetime;
- sparse metadata trees;
- cross-object transactions;
- automatic coarse-to-fine token splitting;
- independent block reclamation.

These features require additional semantic or recovery machinery and should not
be hidden inside the first granularity change.
