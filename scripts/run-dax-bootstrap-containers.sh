#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/run-dax-bootstrap-containers.sh [container-count]
# Host zero publishes the bootstrap first. Other hosts then attach and validate
# the same /dev/dax0.0 region. Each container builds privately under /tmp.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/host-count.sh"
CONTAINER_COUNT="$(cxloom_resolve_host_count "${1:-}")"

if ! [[ "${CONTAINER_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "container-count must be a positive integer" >&2
    exit 1
fi

run_host() {
    local host_id="$1"
    local container_name="cxloom-h${host_id}"
    echo "== DAX bootstrap on ${container_name} =="
    docker exec "${container_name}" bash -lc '
        cmake -S /workspace -B /tmp/cxloom-build -G Ninja
        cmake --build /tmp/cxloom-build --target cxloom_dax_bootstrap --parallel
        /tmp/cxloom-build/cxloom_dax_bootstrap
    '
}

run_host 0
for ((host_id = 1; host_id < CONTAINER_COUNT; ++host_id)); do
    run_host "${host_id}"
done
