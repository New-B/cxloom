# Object and Coherence-Block Design

## 1. Decision

LoomMem manages allocations in the global shared CXL region. Regular shared
files provide the same mapping and allocator path for tests. Application-private
DRAM allocation belongs to the application and operating system.

LoomMem separates object lifetime from coherence granularity:

- an **object** is the unit of allocation, addressing, ownership of lifetime,
  and reclamation;
- a **coherence block** is the unit of write-token arbitration, versioning,
  stable writeback, replica caching, and contention.

An object contains one or more fixed-size coherence blocks. The block size is
selected when the object is allocated and cannot change during that lifetime.
Full-object read/write helpers cover all blocks with the same
per-block semantics as any other range. There is no separate consistency mode.

This separation avoids forcing large objects through one write token without
turning every cache line into a separately allocated object.

## 2. Granularity Rules

The default block size is `CxloomConfig::coherence_granule_bytes`, currently
4 KiB. `AllocationOptions::coherence_block_bytes` may override it per allocation;
zero selects the runtime default.

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
    uint64_t object_offset;
    uint64_t bytes;
    uint64_t alignment;

    uint64_t coherence_block_bytes;
    uint64_t coherence_block_count;
    uint64_t coherence_metadata_offset;
    uint64_t data_extent_offset;
    uint64_t data_extent_bytes;
    uint64_t coherence_extent_bytes;
    atomic<uint64_t> active_references[kMaxHosts];
};
```

Object addresses are valid only while the application-owned lifetime is active.
LoomMem does not attach a generation or allocation identity to an object.
Content versions and writeback epochs live only in block metadata. Object state
and active references coordinate retirement and safe reclamation; they do not
provide data snapshot semantics.

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
<object GPtr, block index>
```

A token lease additionally contains `token_epoch`. Every request, grant, and
release validates:

- the GPtr names a published allocation base;
- `block_index < coherence_block_count`;
- the local host is the authoritative token owner;
- the lease token epoch matches the block descriptor.

The token epoch protects one live block against a stale lease after ownership
transfer. Access after the application lifetime is invalid; the runtime relies
on the retirement protocol to prevent reuse while accepted activity drains.

## 5. Range API

Applications express byte ranges rather than block indices:

```cpp
AcquireReadRange(object, offset, bytes, timeout);
AcquireWriteRange(object, offset, bytes, timeout);
```

The runtime validates overflow and object bounds, then maps the byte range to
an inclusive block interval:

```text
first = offset / block_bytes
last  = (offset + bytes - 1) / block_bytes
```

Full-range convenience wrappers:

```cpp
AcquireReadSnapshot(object, timeout)
AcquireWriteBuffer(object, timeout)
```

They cover `[0, object_bytes)` with per-block validation and publication.

A returned range snapshot is contiguous and immutable in V1. It records the
    ordered vector of participating block versions. Each version is meaningful
only for that live block. The result is immutable but
does not promise a common point-in-time snapshot across blocks.
Internally the cache is block-based; the runtime assembles the requested byte
range from stable block replicas. A later scatter/gather view may avoid this
final copy.

## 6. Block Read Protocol

The local replica key becomes:

```text
<object offset, block index>
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
Applications coordinate cross-block invariants using their own synchronization
protocols. LoomMem does not validate or publish a range as one transaction.

## 7. Block Write Protocol

Writers operate on host-private buffers. For one block:

```text
request block token
copy latest committed block to private buffer
modify private buffer while writeback_epoch remains even
set writeback_epoch odd
copy changed bytes to CXL and publish them
increment block version
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

## 9. Multi-Block Publication

Each block is published and versioned independently. Readers may observe a mix
of old and new block versions, including when a read covers the entire object.
A failed multi-block publication may leave earlier blocks committed; it has no
range-wide rollback guarantee. Acquiring all required tokens before modifying
data serializes conflicting writers but does not make publication atomic for
readers.

## 10. Allocation and Publication

Creating a shared object becomes:

1. validate object size, alignment, and block size;
2. calculate `block_count` with checked arithmetic;
3. allocate a fresh dense block-descriptor extent from the coherence free pool;
4. allocate a fresh descriptor-and-data extent from the data free pool;
5. initialize every block with the allocating host as token owner, version 0,
   token epoch 1, and writeback epoch 0;
6. fill the allocation descriptor with the sidecar offset, dimensions, extent
   information, and initially empty activity counters;
7. publish block metadata, then publish the allocation state last.

Failure before publication rolls both extent reservations back into their free
pools.

Attaching hosts validate the bootstrap, allocator, and coherence-region layout
versions before resolving an allocation.

## 11. Reclamation Contract

The detailed all-host closing, queue-watermark draining, cache cleaning, retry,
and space-reuse protocol is specified in `docs/object-retirement.md`.

Data and all block metadata share one object lifetime. Blocks cannot be freed
or reused independently.

Reclamation uses these states:

```text
ALLOCATED -> RETIRING -> FREE
```

`RETIRING` rejects new reads, object references, and token requests. Existing
writers retain their references and may finish publication. Reclamation waits until:

- no block has an active write lease;
- every writeback epoch is even;
- every host's active-reference slot is zero;
- host-local cache entries for the retired object may only survive as detached
  immutable snapshots.

Only then does the descriptor cease to exist and both extents return to their
independent free pools. Any later access through the old object handle is an
application lifetime error.

## 12. Implementation and Layout Compatibility

- Runtime defaults and per-allocation block-size options select the fixed block
  size. Small objects naturally occupy one block.
- The allocation descriptor and dense block sidecars are separate. No
  authoritative data version or token state remains in the object descriptor.
- Token messages, leases, arbitration, versions, writeback epochs, and the
  bounded replica LRU use block identity.
- Byte-range operations acquire tokens in canonical order, roll back partial
  acquisition, and assemble immutable results from independently stable blocks.
- Full-object helpers use exactly the same range implementation and guarantees.
- Object-wide reclamation retires the allocation, drains block requests and
  active references, invalidates the descriptor, and returns data and sidecar
  extents to separate split/coalesce pools.
- Bootstrap layout version 11 and allocator layout version 10 identify the
  current descriptor layout. Existing shared regions must be reinitialized;
  attaching to an older layout is rejected.

## 13. Required Tests

Validation covers allocator, queue, token, visibility, and coherence behavior,
including:

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
