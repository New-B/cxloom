#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/host-count.sh"
HOST_COUNT="$(cxloom_resolve_host_count "${1:-}")"
ITERATIONS="${CL_COHERENCE_ITERATIONS:-1000}"
TIMEOUT_MS="${CL_COHERENCE_TIMEOUT_MS:-120000}"

cxloom_validate_host_count "$HOST_COUNT" 2
for value in "$ITERATIONS" "$TIMEOUT_MS"; do
    if ! [[ "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "coherence stress parameters must be positive integers" >&2
        exit 1
    fi
done

build_pids=()
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    (
        docker exec "cxloom-h${host_id}" bash -lc '
            cmake -S /workspace -B /tmp/cxloom-build -G Ninja
            cmake --build /tmp/cxloom-build --target cxloom_coherence_stress --parallel
        '
    ) >"/tmp/cxloom-coherence-build-${host_id}.log" 2>&1 &
    build_pids+=("$!")
done
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    if ! wait "${build_pids[$host_id]}"; then
        sed -n '1,200p' "/tmp/cxloom-coherence-build-${host_id}.log"
        exit 1
    fi
done

start_host() {
    local host_id="$1"
    docker exec --env "CL_HOST_COUNT=${HOST_COUNT}" --env "CL_COHERENCE_ITERATIONS=${ITERATIONS}" --env "CL_COHERENCE_TIMEOUT_MS=${TIMEOUT_MS}" "cxloom-h${host_id}" /tmp/cxloom-build/cxloom_coherence_stress >"/tmp/cxloom-coherence-stress-${host_id}.log" 2>&1 &
    last_pid=$!
}

pids=()
start_host 0
pids+=("$last_pid")
owner_ready=0
for _ in {1..600}; do
    if grep -q 'initialized shared CXL region' /tmp/cxloom-coherence-stress-0.log; then
        owner_ready=1
        break
    fi
    if ! kill -0 "${pids[0]}" 2>/dev/null; then break; fi
    sleep 0.05
done
if (( owner_ready == 0 )); then
    wait "${pids[0]}" || true
    sed -n '1,200p' /tmp/cxloom-coherence-stress-0.log
    exit 1
fi
for ((host_id = 1; host_id < HOST_COUNT; ++host_id)); do
    start_host "$host_id"
    pids+=("$last_pid")
done

result=0
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    if ! wait "${pids[$host_id]}"; then result=1; fi
    echo "== coherence stress host=${host_id} =="
    sed -n '1,200p' "/tmp/cxloom-coherence-stress-${host_id}.log"
done
exit "$result"
