# LoomMem regression baseline — 2026-09-05

Source revision: `2e3e1eba2a05c978e77420b5b42021508324d1b0`.
The source working tree was clean before testing. No functional code changed.

Platform: one Intel Xeon Gold 6448H server, four sockets / compute NUMA nodes,
128 physical cores; `/dev/dax0.0`, 1 GiB mapped per runtime. Sixteen containers,
eight distinct physical cores each, NUMA-local memory affinity, spread across
nodes 0–3. This is the existing single-machine coherent emulation profile.

| Regression | Parameters | Result |
| --- | --- | --- |
| Configure/build and CTest | All 12 registered tests; 180 s per-test timeout | 12/12 passed |
| Container affinity | 16 hosts | Passed |
| DAX initialization / allocation | 16 hosts, 4 allocations per host | All hosts joined; non-overlap and remote reads passed |
| All-pairs queue transport | 240 directed queues, 100,000 iterations | All 16 hosts: errors=0 |
| Token stress | 16 hosts, 10,000 iterations, batch 32 | All 16 hosts: errors=0 |
| Coherence stress | 16 hosts, 1,000 iterations | All 16 hosts: final_version=1000, errors=0 |
| Visibility matrix | release, seq_cst, clflush, clwb; 100,000 iterations each | All 64 host/mode results: local_errors=0, total_errors=0 |

All six container-script stages returned exit status zero. The visibility
result supports retaining release/acquire for this platform. This is one
regression run, not a universal ordering proof or a performance benchmark.

## Reproduction and evidence

```bash
cmake -S . -B build
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --timeout 180
./scripts/launch-numa-containers.sh 16
./scripts/verify-numa-containers.sh 16
./scripts/run-host-init-containers.sh 16
./scripts/run-queue-transport-containers.sh 16
./scripts/run-token-stress-containers.sh 16
./scripts/run-coherence-stress-containers.sh 16
CL_VISIBILITY_ITERATIONS=100000 ./scripts/run-visibility-litmus-containers.sh 16
```

Run shared-DAX tests sequentially: each owner initializes a fresh test session.
Local raw logs, revision, CPU/container profiles and SHA-256 checksums are in
`run/baseline-20260905/` (git-ignored); the CTest log is copied there too.
The sixteen test containers remain available for subsequent LoomPar work.

## Real non-coherent multi-host validation: deferred

The project currently has no physical non-coherent multi-host platform. On
2026-09-05, the user explicitly agreed to defer this validation and proceed
with single-machine multi-NUMA container emulation as the development baseline.
This deferred item does not block LoomPar lifecycle development.
Running CLFLUSH/CLWB on this coherent server does not emulate the absence of
hardware coherence, so those successful runs do not close this requirement.

On that platform, first establish that distinct physical hosts map the same
shared region and identify its coherence domain. Then run the visibility matrix
repeatedly with coordinated owner startup, record all host results and hardware
constraints, select a supported publication profile, and rerun queue, token and
coherence integration tests using that profile. Do not inherit release/acquire
from this machine without validation.

The current-platform baseline is suitable for starting LoomPar lifecycle work;
real non-coherent hardware support remains unvalidated.
