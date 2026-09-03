# NUMA Container Environment

The target server has four compute NUMA nodes and exposes the shared CXL capacity as /dev/dax0.0. A CXLoom logical host is a container; multiple containers may use disjoint core sets on the same NUMA node.

| Logical hosts | Placement | Default cores per host | DRAM limit per host |
| --- | --- | --- | --- |
| 0-15 | round-robin across NUMA nodes 0-3 | 8 physical cores | 32 GiB |

CPUs 128-255 are hyperthread siblings of CPUs 0-127 and are deliberately not
included. Docker does not disable hyperthreading globally; this CPU restriction
gives CXLoom workers one hardware thread per physical core.

## Start

Run on the Linux CXL server from the repository root:

```bash
./scripts/build-container.sh
./scripts/launch-numa-containers.sh 16
./scripts/verify-numa-containers.sh 16
./scripts/run-smoke-containers.sh 16
```

The launch count is bounded dynamically by the available physical cores; with the default 8 cores per container, the target server supports 1 to 16 containers. To change defaults later:

```bash
CXLOOM_CPUS_PER_CONTAINER=8 \
CXLOOM_CONTAINER_MEMORY=32g \
CXLOOM_SHARED_REGION_BYTES=8G \
./scripts/launch-numa-containers.sh 16
```

Use `./scripts/stop-numa-containers.sh` to remove these containers.

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

## Updated Logical-Host Model

The logical-host boundary is the container, not the physical NUMA node. By
default each container receives eight physical cores and a 32 GiB memory limit,
with its CPU and ordinary DRAM affinity constrained to one NUMA node. Multiple
containers may occupy disjoint core sets on the same NUMA node. Placement is
round-robin by `host_id % NUMA_NODE_COUNT`; on the current four-NUMA,
128-physical-core server this supports up to 16 eight-core logical hosts.

Use the following test for the initial multi-host LoomMem system bring-up:

```bash
./scripts/run-host-init-containers.sh 16
```

Unlike the older sequential mapping check, this keeps all logical hosts alive
concurrently. Host zero initializes a fresh DAX bootstrap header, each host
registers and publishes a per-host probe, and every host waits for and validates
all configured peers.
