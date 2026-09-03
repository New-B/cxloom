# NUMA Container Environment

The target server exposes compute NUMA nodes 0-3 and a CPU-less CXL memory
node 4. The scripts model one CXLoom logical host per compute node.

| Logical host | NUMA node | Default CPU set | Allowed memory nodes |
| --- | --- | --- | --- |
| 0 | 0 | 0-15 | 0,4 |
| 1 | 1 | 32-47 | 1,4 |
| 2 | 2 | 64-79 | 2,4 |
| 3 | 3 | 96-111 | 3,4 |

CPUs 128-255 are hyperthread siblings of CPUs 0-127 and are deliberately not
included. Docker does not disable hyperthreading globally; this CPU restriction
gives CXLoom workers one hardware thread per physical core.

## Start

Run on the Linux CXL server from the repository root:

```bash
./scripts/build-container.sh
./scripts/launch-numa-containers.sh 4
./scripts/verify-numa-containers.sh 4
./scripts/run-smoke-containers.sh 4
```

The launch count is parameterized from 1 to 4. To change defaults later:

```bash
CXLOOM_CPUS_PER_CONTAINER=24 \
CXLOOM_CONTAINER_MEMORY=48g \
CXLOOM_SHARED_REGION_BYTES=8G \
./scripts/launch-numa-containers.sh 4
```

Use `./scripts/stop-numa-containers.sh 4` to remove these containers.

## CXL Backing Caveat

Empty `cxl list -M -u` and `daxctl list` output is expected when node 4 has
been onlined as Linux system RAM rather than exposed as a DAX device. Scripts
therefore mount `run/cxl-shared` at `/cxloom-shared` and export
`CL_SHARED_BACKING` for the future `mmap(MAP_SHARED)` region mapper.

The launcher now passes `/dev/dax0.0` through and exports `CL_DAX_DEVICE` plus
`CL_BOOTSTRAP_OWNER`. Applications must pass these values into `cl_config_t`.
LoomMem can map the DAX device and establish its shared bootstrap header, but
allocator metadata and queues remain process-local. Cross-container allocation,
queue transport, and coherence are therefore not available yet.

After launching containers, verify the real DAX mapping and bootstrap protocol:

```bash
./scripts/run-dax-bootstrap-containers.sh 4
```

The script runs host zero first with `CL_BOOTSTRAP_OWNER=1`, then starts the
remaining hosts in attach mode. Each line prints a host ID, its local mapping
base, and the common shared-data offset. Bases may differ; the offset must be
identical for every host.
