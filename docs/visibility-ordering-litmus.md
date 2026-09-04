# CXL Visibility and Ordering Litmus

This harness measures which publication sequence is sufficient on the target
shared devdax platform. It does not infer hardware guarantees from ordinary
file-backed mmap behavior.

## Protocol

Every logical host allocates one 4 KiB multi-cache-line record from the global
shared allocator and publishes its GPtr. For every iteration, every host:

1. rewrites the entire local record with a sequence-dependent pattern;
2. applies the selected data-publication strategy;
3. release-publishes its iteration control word;
4. applies the selected acquire strategy while waiting for every host publication
   word;
5. applies the selected acquire strategy to each remote record, then validates
   both sequence markers, writer ID, checksum, and every payload word in every
   host.s record;
6. release-publishes its observation word;
7. waits for every observer before allowing the next rewrite.

The second barrier prevents a fast writer from overwriting iteration N before a
slow reader has inspected it.

## Strategies

- release: C++ release/acquire fences around control publication and data consumption.
- seq_cst: sequentially consistent fences on both sides.
- clflush: producer CLFLUSH plus MFENCE; consumer CLFLUSH invalidation plus MFENCE.
- clwb: producer CLWB plus SFENCE; consumer CLFLUSH invalidation plus MFENCE.
  The executable rejects this mode if CPUID does not advertise CLWB.

For flush modes, the producer also flushes each published control word after
its release store. The consumer invalidates a control word before polling it and
invalidates each data record before validation. This makes the tested recipe
explicit on a platform where CPU caches are not coherent across hosts.

## Running

After launching the containers:

    ./scripts/run-visibility-litmus-containers.sh 16

The default is 1,000 iterations for every strategy. Longer experiments can use:

    CL_VISIBILITY_ITERATIONS=100000 \
    ./scripts/run-visibility-litmus-containers.sh 16

Select a subset when a CPU does not implement CLWB:

    CL_VISIBILITY_MODES="release seq_cst clflush" \
    ./scripts/run-visibility-litmus-containers.sh 16

A successful line looks like:

    host=0 mode=clwb iterations=100000 elapsed_ms=1234 local_errors=0 total_errors=0

Any non-zero error count or timeout makes that strategy fail.

## Interpreting Results

Passing a finite run is evidence for the tested platform and workload, not a
proof of a universal CXL ordering rule. Run each strategy repeatedly at the
largest host count and iteration count practical. The weakest strategy with
repeatable zero-error results is a candidate publication recipe; retain a
stronger strategy if platform documentation requires it.

File-backed local tests validate the harness, allocator, barriers, and error
detection only. The decision used by LoomMem must be based on /dev/dax0.0
results from the target CXL server.

## Measured Platform Profile (2026-09-04)

The current Intel Xeon Gold 6448H machine was tested with two processes pinned
to NUMA nodes 0 and 1 while mapping `/dev/dax0.0` (128 GiB, target node 4).
All four recipes completed 1,000 iterations with zero errors. The weakest
`release` recipe then completed 100,000 iterations with zero errors.

For this single-machine, CPU-cache-coherent emulation profile, release/acquire
is therefore the selected minimum recipe; cache-line flushes only add overhead.
This is an empirical platform result, not evidence that release/acquire is
sufficient on a physically non-coherent multi-host CXL system. Such hardware
must rerun this matrix, and LoomMem must select the resulting platform profile
rather than silently inheriting this one.

