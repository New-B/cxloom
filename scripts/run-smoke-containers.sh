#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/run-smoke-containers.sh [container-count]
# Validates build and shared-memory behavior using a temporary shared file per container.
# Real CXL validation is provided by run-host-init-containers.sh.
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
        smoke_region=$(mktemp /tmp/cxloom-smoke-XXXXXX)
        cleanup_smoke() { rm -f "$smoke_region"; }
        trap cleanup_smoke EXIT
        CL_HOST_ID=0 CL_HOST_COUNT=1 /tmp/cxloom-build/cxloom_smoke "$smoke_region"
    '
done
