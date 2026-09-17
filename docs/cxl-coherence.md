# LoomMem Single-Writer/Multi-Reader Coherence

> The implemented read path is specified in [Read Access under Release Consistency](read-access-design.md).

Each shared allocation has an ephemeral allocation descriptor, CXL-resident
per-block authoritative metadata, a per-object admission lock, shared
replica-host membership, independent per-host active-operation counters, and
host-private immutable block replicas.

## Public operations

- AcquireWriteRange acquires the affected block tokens in ascending order and
  refreshes a private mutable buffer from CXL.
- ReleaseWriteBuffer publishes each affected block separately, increments that
  block's version, ends its writeback epoch, and releases its token.
- AcquireReadRange returns immutable storage plus an ordered block-version
  vector. Current-index hits read DRAM directly; old-index hits are validated
  against CXL on demand after an acquire boundary, and misses fill from CXL.
- AcquireReadSnapshot and AcquireWriteBuffer are convenience wrappers covering
  the complete allocation. They use the same per-block guarantees.

ReadSnapshot storage is reference-counted. Refresh replaces a cache entry
instead of modifying an existing replica. Retained results remain immutable
and memory-safe; a multi-block result does not promise a common point in time.

The host-local replica cache is bounded by both entry count and total bytes
(`replica_cache_capacity_entries` and `replica_cache_capacity_bytes`) and uses
LRU replacement. Eviction removes only the runtime's cache reference; snapshots
already held by applications remain valid through shared ownership. A block
larger than the byte budget can still be returned as a snapshot but is evicted
immediately instead of remaining resident. Write buffers are caller-owned and
are never cache eviction candidates while they hold a write token. Local
publication invalidates the affected cached blocks in both indexes;
subsequent reads fill the newly committed version. Both indexes share the same
LRU capacity and one host membership per cached object.

## Stable-copy protocol

Each block sidecar's writeback epoch is even while that block is stable and odd
only during writeback. Acquiring a buffered write token leaves it even because
the application modifies a host-private buffer. Release makes the affected
block epoch odd immediately before copying, publishes data, increments its
version, then restores an even epoch.
Consequently, readers can continue to acquire the last committed version while
a writer holds and modifies its private buffer.

A reader accessing CXL (a current-index hit skips this entire protocol):

1. registers an active operation through a stable lookup slot and object lock;
2. waits while the block writeback epoch is odd;
3. records the block epoch and block version;
4. copies the shared block bytes;
5. acquires sidecar metadata again;
6. accepts the copy only when epoch and version are unchanged and the epoch is
   even; otherwise it retries.

This prevents a reader from accepting a torn writeback while allowing readers
on different hosts to consume immutable snapshots concurrently.

## Scope

The protocol operates at configurable block granularity. A new read may
return the last committed version while a buffered writer holds the token, and
a cache miss waits or retries during the writer's actual CXL writeback window.
Within one synchronization interval a cached block may continue to return an
older committed version. `SynchronizeAcquire` rotates current/old indexes;
the next access validates old blocks and refreshes changed ones. Already
returned immutable snapshots remain valid independently of cache lifetime. A timed-out
synchronous write acquisition abandons its request; a late grant is released
without changing the block version. Cross-block invariants require application
synchronization. Host failure recovery remains future work.

The three-runtime unit test covers parallel readers, reading the last committed
version while a buffered writer is active, stale replica refresh, immutable old
snapshots, and concurrent reuse of one cached version. The variable-scale devdax test rotates one writer
across all configured containers while every host validates read snapshots and
the final version of the single block containing the test record.
