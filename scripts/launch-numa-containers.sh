#!/usr/bin/env bash
set -euo pipefail

# NUMA / CXL deployment model:
# - After switching CXL capacity to devdax, the CXL region is exposed as /dev/dax0.0.
# - It is no longer a separate memory-only NUMA node in the normal Linux page allocator.
# - Therefore, the container's memory affinity and CPU affinity should be kept aligned on
#   the same host NUMA node, while the CXL backing is passed through as a device and used
#   explicitly by applications.
# - We discover the available host NUMA nodes at runtime via numactl --hardware and map each
#   container onto a compute NUMA node using host_id % NUMA_NODE_COUNT.
# - This keeps container placement balanced across NUMA nodes without hard-coding a node count.
#
# Example on a 128-core host (4 NUMA nodes, ~32 physical cores per node).
# Mapping policy used by this script:
#   - compute_numa_node = host_id % NUMA_NODE_COUNT  (round-robin across nodes)
#   - local_index = floor(host_id / NUMA_NODE_COUNT)  (which chunk inside the node)
#   - each container receives CXLOOM_CPUS_PER_CONTAINER contiguous physical cores
#     from the node's primary-CPU list (one CPU id per physical core, hyperthreads excluded)
#
# Example assignments (host_id -> node, CPUs) for CXLOOM_CPUS_PER_CONTAINER=8:
#   host_id=0  -> node0, CPUs 0-7
#   host_id=1  -> node1, CPUs 32-39
#   host_id=2  -> node2, CPUs 64-71
#   host_id=3  -> node3, CPUs 96-103
#   host_id=4  -> node0, CPUs 8-15
#   host_id=5  -> node1, CPUs 40-47
#   ...
#   host_id=15 -> node3, CPUs 120-127
#
# This corresponds to 16 containers × 8 physical cores/container = 128 total physical cores.
#
# Important runtime semantics:
#   - The container's compute NUMA node is selected by host_id % NUMA_NODE_COUNT.
#   - This keeps each container's CPU and memory affinity aligned on the same host node.
#   - The CXL backing is not treated as a separate operating-system NUMA node after devdax
#     mode is enabled; it is passed into the container as /dev/dax0.0 and used by apps.
#   - The device group id 986 corresponds to the cxlmem group created on the host so the
#     container user can access /dev/dax0.0 without root privileges.
#
# Usage: ./scripts/launch-numa-containers.sh [container-count]
# CONTAINER_COUNT: number of containers to launch. It must not exceed the host capacity
# because each container is mapped onto NUMA-local physical cores.
CONTAINER_COUNT="${1:-${CL_HOST_COUNT:-}}"
CXLOOM_IMAGE="${CXLOOM_IMAGE:-cxloom:dev}"
# With 128 physical cores and hyperthreading disabled, a full-utilization run uses
# 16 containers × 8 CPUs/container. Keep the count as an argument to the script, but
# the default per-container allocation remains 8 cores to match the target topology.
CXLOOM_CPUS_PER_CONTAINER="${CXLOOM_CPUS_PER_CONTAINER:-8}"
CXLOOM_CONTAINER_MEMORY="${CXLOOM_CONTAINER_MEMORY:-32g}"
CXLOOM_SHARED_REGION_BYTES="${CXLOOM_SHARED_REGION_BYTES:-1G}"
# cxlmem is the device group that owns /dev/dax0.0 on the host. The container joins this group
# so applications running as a normal user can open the device through the passed-through node.
CXLMEM_GID=986

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SHARED_DIR="${ROOT_DIR}/run/cxl-shared"

# Auto-discover the host NUMA topology at runtime so the script works on other machines.
# After switching CXL capacity to devdax, there is no separate CXL-only NUMA node to reserve.
NUMA_NODE_COUNT="$(numactl --hardware 2>/dev/null | awk '/available:/ {print $2}' || echo 1)"
if ! [[ "${NUMA_NODE_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
    NUMA_NODE_COUNT=1
fi

HOST_PHYSICAL_CPU_COUNT="$(nproc --all 2>/dev/null || echo 1)"
if ! [[ "${HOST_PHYSICAL_CPU_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
    HOST_PHYSICAL_CPU_COUNT=1
fi

if ! [[ "${CONTAINER_COUNT}" =~ ^[1-9][0-9]*$ ]] || (( CONTAINER_COUNT > 64 )); then
    echo "container-count must be provided and be an integer from 1 to 64" >&2
    exit 1
fi
if ! [[ "${CXLOOM_CPUS_PER_CONTAINER}" =~ ^[1-9][0-9]*$ ]] || (( CXLOOM_CPUS_PER_CONTAINER > 32 )); then
    echo "CXLOOM_CPUS_PER_CONTAINER must be between 1 and 32" >&2
    exit 1
fi

MAX_CONTAINERS=$((HOST_PHYSICAL_CPU_COUNT / CXLOOM_CPUS_PER_CONTAINER))
if (( CONTAINER_COUNT > MAX_CONTAINERS )); then
    echo "container-count ${CONTAINER_COUNT} exceeds the host capacity for ${CXLOOM_CPUS_PER_CONTAINER} CPUs/container (${MAX_CONTAINERS} containers max on this host)" >&2
    exit 1
fi
# Build a per-NUMA-node list of physical (one thread per core) CPU IDs.
# We use `lscpu -p=CPU,CORE,NODE` and pick the lowest CPU id for each (node,core)
# pair so that hyperthread siblings are not double-counted. This yields a stable
# primary-CPU list for each NUMA node which we use to allocate contiguous
# CXLOOM_CPUS_PER_CONTAINER slices inside the node.
declare -A _min_cpu_for
declare -A NODE_PHYSICAL_CPUS
if command -v lscpu >/dev/null 2>&1; then
    while IFS=, read -r cpu core node; do
        [[ "$cpu" =~ ^# ]] && continue
        key="${node}_${core}"
        prev=${_min_cpu_for[$key]:-}
        if [[ -z "$prev" || $cpu -lt $prev ]]; then
            _min_cpu_for[$key]=$cpu
        fi
    done < <(lscpu -p=CPU,CORE,NODE 2>/dev/null)

    # Aggregate per-node
    for k in "${!_min_cpu_for[@]}"; do
        node=${k%%_*}
        cpu=${_min_cpu_for[$k]}
        NODE_PHYSICAL_CPUS[$node]="${NODE_PHYSICAL_CPUS[$node]:-}${cpu} "
    done

    # Sort each node's cpu list
    for n in $(seq 0 $((NUMA_NODE_COUNT - 1))); do
        list=${NODE_PHYSICAL_CPUS[$n]:-}
        if [[ -n "$list" ]]; then
            NODE_PHYSICAL_CPUS[$n]=$(echo $list | tr ' ' '\n' | grep -E '^[0-9]+' | sort -n | tr '\n' ' ')
        else
            NODE_PHYSICAL_CPUS[$n]=''
        fi
    done
else
    # Fallback: use /sys/devices/system/node/node*/cpulist and prefer lower ids
    for n in $(seq 0 $((NUMA_NODE_COUNT - 1))); do
        path="/sys/devices/system/node/node${n}/cpulist"
        if [[ -r "$path" ]]; then
            raw=$(cat "$path")
            # take only the lower half of the ranges to avoid hyperthreads when possible
            NODE_PHYSICAL_CPUS[$n]=$(echo "$raw" | awk -F, '{for(i=1;i<=NF;i++) { if ($i ~ /-/) {split($i,a,"-"); for(j=a[1]; j<=a[2]; j++) print j} else print $i}}' | sort -n | uniq | tr '\n' ' ')
        else
            NODE_PHYSICAL_CPUS[$n]=''
        fi
    done
fi

# Calculate how many containers fit per node and overall using physical CPUs
TOTAL_MAX_CONTAINERS=0
for n in $(seq 0 $((NUMA_NODE_COUNT - 1))); do
    read -a arr <<< "${NODE_PHYSICAL_CPUS[$n]:-}"
    pernode=$(( ${#arr[@]} / CXLOOM_CPUS_PER_CONTAINER ))
    TOTAL_MAX_CONTAINERS=$(( TOTAL_MAX_CONTAINERS + pernode ))
done
if (( CONTAINER_COUNT > TOTAL_MAX_CONTAINERS )); then
    echo "container-count ${CONTAINER_COUNT} exceeds host per-NUMA capacity (${TOTAL_MAX_CONTAINERS} containers max respecting per-node packing)" >&2
    exit 1
fi
if ! docker image inspect "${CXLOOM_IMAGE}" >/dev/null 2>&1; then
    echo "Docker image ${CXLOOM_IMAGE} is missing. Run ./scripts/build-container.sh first." >&2
    exit 1
fi

mkdir -p "${SHARED_DIR}"

for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    # Allocate CXLOOM_CPUS_PER_CONTAINER physical CPUs from the target NUMA node.
    # Policy: containers are round-robin across NUMA nodes by host_id; the local index
    # within the node is computed as floor(host_id / NUMA_NODE_COUNT). For node N we
    # allocate a contiguous chunk of the node's primary CPU list.
    compute_numa_node=$((host_id % NUMA_NODE_COUNT))

    # get per-node primary cpu list (space-separated)
    node_list="${NODE_PHYSICAL_CPUS[$compute_numa_node]:-}"
    read -a node_cpus <<< "$node_list"
    local_index=$(( host_id / NUMA_NODE_COUNT ))
    start_idx=$(( local_index * CXLOOM_CPUS_PER_CONTAINER ))
    end_idx=$(( start_idx + CXLOOM_CPUS_PER_CONTAINER - 1 ))
    if (( end_idx >= ${#node_cpus[@]} )); then
        echo "not enough physical CPUs on node ${compute_numa_node} to satisfy container ${host_id}" >&2
        exit 1
    fi
    cpu_chunk=( "${node_cpus[@]:start_idx:CXLOOM_CPUS_PER_CONTAINER}" )
    # join with comma for Docker cpuset
    cpu_set=$(IFS=,; echo "${cpu_chunk[*]}")
    container_name="cxloom-h${host_id}"

    if docker container inspect "${container_name}" >/dev/null 2>&1; then
        echo "Container ${container_name} already exists; stop it first with scripts/stop-numa-containers.sh." >&2
        exit 1
    fi


    docker run --detach \
        --name "${container_name}" \
        --hostname "${container_name}" \
        --cpuset-cpus "${cpu_set}" \
        --cpuset-mems "${compute_numa_node}" \
        --memory "${CXLOOM_CONTAINER_MEMORY}" \
        --memory-swap "${CXLOOM_CONTAINER_MEMORY}" \
        --cap-add SYS_NICE \
        --ulimit memlock=-1:-1 \
        --device=/dev/dax0.0:/dev/dax0.0:rwm \
        --group-add "${CXLMEM_GID}" \
        --mount "type=bind,src=${ROOT_DIR},dst=/workspace" \
        --mount "type=bind,src=${SHARED_DIR},dst=/cxloom-shared" \
        --env "CL_HOST_ID=${host_id}" \
        --env "CL_HOST_COUNT=${CONTAINER_COUNT}" \
        --env "CL_COMPUTE_NUMA_NODE=${compute_numa_node}" \
        --env "CL_DAX_DEVICE=/dev/dax0.0" \
        --env "CL_BOOTSTRAP_OWNER=$([[ ${host_id} -eq 0 ]] && echo 1 || echo 0)" \
        --env "CL_SHARED_BACKING=/cxloom-shared/cxloom.region" \
        --env "CL_SHARED_REGION_BYTES=${CXLOOM_SHARED_REGION_BYTES}" \
        "${CXLOOM_IMAGE}" sleep infinity

    echo "Started ${container_name}: CPUs ${cpu_set}, memory node ${compute_numa_node}."
done
