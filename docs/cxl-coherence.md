# LoomMem Single-Writer/Multi-Reader Coherence

Each shared allocation has CXL-resident authoritative metadata and host-private
immutable read replicas.

## Public operations

- AcquireWriteBuffer requests the object's write token, refreshes a private
  mutable buffer from CXL, and returns it with the token lease.
- ReleaseWriteBuffer copies that buffer to CXL, publishes it, increments the
  global version, ends the write epoch, and optionally hands the token to the
  next requester.
- AcquireReadSnapshot returns an immutable shared pointer plus its version. A
  matching cached version is reused; a stale or absent replica is copied from
  CXL and installed in the host-local cache.

ReadSnapshot storage is reference-counted. Refresh replaces the cache entry
instead of modifying an existing snapshot, so readers already using an older
snapshot remain memory-safe and internally consistent.

The host-local replica cache is bounded by both entry count and total bytes
(`replica_cache_capacity_entries` and `replica_cache_capacity_bytes`) and uses
LRU replacement. Eviction removes only the runtime's cache reference; snapshots
already held by applications remain valid through shared ownership. An object
larger than the byte budget can still be returned as a snapshot but is evicted
immediately instead of remaining resident. Write buffers are caller-owned and
are never cache eviction candidates while they hold a write token. The
immutable replica installed after a successful release is subject to the same
LRU limits as reader-created replicas.

## Stable-copy protocol

AllocationDescriptor coherence_epoch is even while shared object bytes are
stable and odd only during writeback to those bytes. Acquiring a buffered write
token leaves the epoch even because the application modifies a host-private
buffer. Release moves the epoch to odd immediately before copying that buffer
to CXL, publishes data, increments version, then moves the epoch back to even.
Consequently, readers can continue to acquire the last committed version while
a writer holds and modifies its private buffer.

A reader:

1. acquires descriptor metadata;
2. waits while coherence_epoch is odd;
3. records coherence_epoch, generation, and version;
4. copies the shared bytes;
5. acquires descriptor metadata again;
6. accepts the copy only when epoch and version are unchanged and the epoch is
   even; otherwise it retries.

This prevents a reader from accepting a torn writeback while allowing readers
on different hosts to consume immutable snapshots concurrently.

## Scope

The protocol currently operates at allocation granularity. A new read may
return the last committed version while a buffered writer holds the token, and
only waits or retries during the writer's actual CXL writeback window. An
already acquired immutable snapshot may be used until its owner explicitly
acquires another snapshot. A timed-out
synchronous write acquisition abandons its request; a late grant is released
without changing the object version. Failure recovery, eviction policy,
sub-allocation coherence granules, and automatic refresh of previously
returned pointers are future work.

The three-runtime unit test covers parallel readers, reading the last committed
version while a buffered writer is active, stale replica refresh, immutable old
snapshots, and concurrent reuse of one cached version. The variable-scale devdax test rotates one writer
across all configured containers while every host validates read snapshots and
the final global version.
