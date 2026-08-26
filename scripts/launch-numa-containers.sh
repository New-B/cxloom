#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/launch-numa-containers.sh [container-count]
# NUMA nodes 0-3 are compute nodes; node 4 is the CPU-less CXL memory node.
CONTAINER_COUNT="${1:-4}"
CXLOOM_IMAGE="${CXLOOM_IMAGE:-cxloom:dev}"
CXLOOM_CPUS_PER_CONTAINER="${CXLOOM_CPUS_PER_CONTAINER:-16}"
CXLOOM_CONTAINER_MEMORY="${CXLOOM_CONTAINER_MEMORY:-32g}"
CXLOOM_SHARED_REGION_BYTES="${CXLOOM_SHARED_REGION_BYTES:-1G}"
CXL_NUMA_NODE=4

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SHARED_DIR="${ROOT_DIR}/run/cxl-shared"

if ! [[ "${CONTAINER_COUNT}" =~ ^[1-4]$ ]]; then
    echo "container-count must be an integer from 1 to 4" >&2
    exit 1
fi
if ! [[ "${CXLOOM_CPUS_PER_CONTAINER}" =~ ^[1-9][0-9]*$ ]] || (( CXLOOM_CPUS_PER_CONTAINER > 32 )); then
    echo "CXLOOM_CPUS_PER_CONTAINER must be between 1 and 32" >&2
    exit 1
fi
if ! docker image inspect "${CXLOOM_IMAGE}" >/dev/null 2>&1; then
    echo "Docker image ${CXLOOM_IMAGE} is missing. Run ./scripts/build-container.sh first." >&2
    exit 1
fi

mkdir -p "${SHARED_DIR}"

for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    # CPUs 0-127 are one hardware thread per physical core. CPUs 128-255 are
    # hyperthread siblings and are intentionally excluded from containers.
    cpu_start=$((host_id * 32))
    cpu_end=$((cpu_start + CXLOOM_CPUS_PER_CONTAINER - 1))
    cpu_set="${cpu_start}-${cpu_end}"
    container_name="cxloom-h${host_id}"

    if docker container inspect "${container_name}" >/dev/null 2>&1; then
        echo "Container ${container_name} already exists; stop it first with scripts/stop-numa-containers.sh." >&2
        exit 1
    fi

    docker run --detach \
        --name "${container_name}" \
        --hostname "${container_name}" \
        --cpuset-cpus "${cpu_set}" \
        --cpuset-mems "${host_id},${CXL_NUMA_NODE}" \
        --memory "${CXLOOM_CONTAINER_MEMORY}" \
        --memory-swap "${CXLOOM_CONTAINER_MEMORY}" \
        --cap-add SYS_NICE \
        --ulimit memlock=-1:-1 \
        --mount "type=bind,src=${ROOT_DIR},dst=/workspace" \
        --mount "type=bind,src=${SHARED_DIR},dst=/cxloom-shared" \
        --env "CL_HOST_ID=${host_id}" \
        --env "CL_HOST_COUNT=${CONTAINER_COUNT}" \
        --env "CL_COMPUTE_NUMA_NODE=${host_id}" \
        --env "CL_CXL_NUMA_NODE=${CXL_NUMA_NODE}" \
        --env "CL_SHARED_BACKING=/cxloom-shared/cxloom.region" \
        --env "CL_SHARED_REGION_BYTES=${CXLOOM_SHARED_REGION_BYTES}" \
        "${CXLOOM_IMAGE}" sleep infinity

    echo "Started ${container_name}: CPUs ${cpu_set}, memory nodes ${host_id},${CXL_NUMA_NODE}."
done
