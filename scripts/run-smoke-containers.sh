#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/run-smoke-containers.sh [container-count]
# This validates build and NUMA placement only. The current runtime does not
# yet map allocator metadata or queues into the shared CXL backing file.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/host-count.sh"
CONTAINER_COUNT="$(cxloom_resolve_host_count "${1:-}")"

if ! [[ "${CONTAINER_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "container-count must be a positive integer" >&2
    exit 1
fi

for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    container_name="cxloom-h${host_id}"
    echo "== building and testing ${container_name} =="
    docker exec "${container_name}" bash -lc '
        cmake -S /workspace -B /tmp/cxloom-build -G Ninja
        cmake --build /tmp/cxloom-build --parallel
        ctest --test-dir /tmp/cxloom-build --output-on-failure
        /tmp/cxloom-build/cxloom_smoke
    '
done
