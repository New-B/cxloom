#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/stop-numa-containers.sh [container-count]
CONTAINER_COUNT="${1:-4}"

if ! [[ "${CONTAINER_COUNT}" =~ ^[1-4]$ ]]; then
    echo "container-count must be an integer from 1 to 4" >&2
    exit 1
fi

for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    container_name="cxloom-h${host_id}"
    if docker container inspect "${container_name}" >/dev/null 2>&1; then
        docker rm --force "${container_name}"
    fi
done
