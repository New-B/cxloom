#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/host-count.sh"
CONTAINER_COUNT="$(cxloom_resolve_host_count "${1:-}")"
if ! [[ "$CONTAINER_COUNT" =~ ^[1-9][0-9]*$ ]] || (( CONTAINER_COUNT > 64 )); then
    echo "container-count must be an integer from 1 to 64" >&2
    exit 1
fi

# Build before starting the timed rendezvous so compilation time cannot make
# early hosts time out while later hosts are still compiling.
build_pids=()
for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    (
        docker exec "cxloom-h${host_id}" bash -lc '
            cmake -S /workspace -B /tmp/cxloom-build -G Ninja
            cmake --build /tmp/cxloom-build --target cxloom_host_init --parallel
        '
    ) >"/tmp/cxloom-host-build-${host_id}.log" 2>&1 &
    build_pids+=("$!")
done
for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    if ! wait "${build_pids[$host_id]}"; then
        sed -n '1,200p' "/tmp/cxloom-host-build-${host_id}.log"
        exit 1
    fi
done

start_host() {
    local host_id="$1"
    docker exec "cxloom-h${host_id}" /tmp/cxloom-build/cxloom_host_init \
        >"/tmp/cxloom-host-init-${host_id}.log" 2>&1 &
    last_pid=$!
}

pids=()
start_host 0
pids+=("$last_pid")

# DAX can retain a Ready header from an earlier run. Do not let attachers read
# it before this run's owner has reset and republished the bootstrap.
owner_ready=0
for _ in {1..200}; do
    if grep -q 'initialized shared CXL region' /tmp/cxloom-host-init-0.log; then
        owner_ready=1
        break
    fi
    if ! kill -0 "${pids[0]}" 2>/dev/null; then break; fi
    sleep 0.05
done
if (( owner_ready == 0 )); then
    wait "${pids[0]}" || true
    sed -n '1,200p' /tmp/cxloom-host-init-0.log
    echo "bootstrap owner did not become ready" >&2
    exit 1
fi

for ((host_id = 1; host_id < CONTAINER_COUNT; ++host_id)); do
    start_host "$host_id"
    pids+=("$last_pid")
done

result=0
for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    if ! wait "${pids[$host_id]}"; then result=1; fi
    echo "== cxloom-h${host_id} =="
    sed -n '1,200p' "/tmp/cxloom-host-init-${host_id}.log"
done
exit "$result"
