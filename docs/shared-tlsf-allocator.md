# Shared CXL TLSF allocator

LoomMem allocates every user-visible object, regardless of size, from the
global shared CXL data pool. It does not maintain host-local object caches or
host-sharded heaps. Allocation and reclamation are protected by the shared
`extent_lock`.

The allocator uses TLSF-style two-level size indexing. Free extents are kept in
two independent pools (object data and coherence sidecars). Each pool has an
address-ordered intrusive list for adjacency checks and size bins plus a
non-empty bitmap for fast selection. Allocation finds a suitable bin, applies
alignment, splits the extent when necessary, and returns the remainder to the
index. Reclamation removes adjacent free nodes, coalesces them, and reinserts
the merged extent. Extent metadata nodes themselves come from a shared free
node list, so node allocation does not scan the node array.

TLSF bins are an internal lookup structure; LoomMem still exposes one unified
allocation interface and does not impose size-specific object semantics.

The consistency sidecar has fixed-size `CoherenceBlockDescriptor` entries.
For an object of `N` bytes and configured block size `B`, the sidecar contains
`ceil(N / B)` entries. The data extent and sidecar extent are allocated
independently, while the object's block mapping and per-block protocol remain
unchanged.

An object in `RETIRING` is never returned to a free bin. It becomes reusable only
after all-host closing, watermark draining, cache cleaning, and the
`RECLAIMABLE` certificate have completed. This keeps allocator policy separate
from the safety protocol that prevents stale cached data from being observed
after reuse.
