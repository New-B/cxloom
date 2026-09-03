#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/verify-numa-containers.sh [container-count]
CONTAINER_COUNT="${1:-4}"

if ! [[ "${CONTAINER_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "container-count must be a positive integer" >&2
    exit 1
fi

for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    container_name="cxloom-h${host_id}"
    echo "== ${container_name} =="
    docker exec "${container_name}" bash -lc '
        echo "CL_HOST_ID=${CL_HOST_ID}"
        echo "CL_COMPUTE_NUMA_NODE=${CL_COMPUTE_NUMA_NODE}"
        taskset -pc $$
        grep -E "Cpus_allowed_list|Mems_allowed_list" /proc/self/status
        numactl --show
    '
done
