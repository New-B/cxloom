#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/host-count.sh"
HOST_COUNT="$(cxloom_resolve_host_count "${1:-}")"
ITERATIONS="${CL_TOKEN_ITERATIONS:-10000}"
BATCH_SIZE="${CL_TOKEN_BATCH_SIZE:-32}"
TIMEOUT_MS="${CL_TOKEN_TIMEOUT_MS:-120000}"
QUEUE_CAPACITY="${CL_QUEUE_CAPACITY:-0}"

cxloom_validate_host_count "$HOST_COUNT" 2
for value in "$ITERATIONS" "$BATCH_SIZE" "$TIMEOUT_MS"; do
    if ! [[ "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "token stress parameters must be positive integers" >&2
        exit 1
    fi
done
if ! [[ "$QUEUE_CAPACITY" =~ ^[0-9]+$ ]]; then
    echo "CL_QUEUE_CAPACITY must be zero (automatic) or a positive integer" >&2
    exit 1
fi

build_pids=()
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    (
        docker exec "cxloom-h${host_id}" bash -lc '
            cmake -S /workspace -B /tmp/cxloom-build -G Ninja
            cmake --build /tmp/cxloom-build --target cxloom_token_stress --parallel
        '
    ) >"/tmp/cxloom-token-build-${host_id}.log" 2>&1 &
    build_pids+=("$!")
done
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    if ! wait "${build_pids[$host_id]}"; then
        sed -n '1,200p' "/tmp/cxloom-token-build-${host_id}.log"
        exit 1
    fi
done

start_host() {
    local host_id="$1"
    docker exec --env "CL_HOST_COUNT=${HOST_COUNT}" --env "CL_TOKEN_ITERATIONS=${ITERATIONS}" --env "CL_TOKEN_BATCH_SIZE=${BATCH_SIZE}" --env "CL_TOKEN_TIMEOUT_MS=${TIMEOUT_MS}" --env "CL_QUEUE_CAPACITY=${QUEUE_CAPACITY}" "cxloom-h${host_id}" /tmp/cxloom-build/cxloom_token_stress >"/tmp/cxloom-token-stress-${host_id}.log" 2>&1 &
    last_pid=$!
}

pids=()
start_host 0
pids+=("$last_pid")
owner_ready=0
for _ in {1..600}; do
    if grep -q 'initialized shared CXL region' /tmp/cxloom-token-stress-0.log; then
        owner_ready=1
        break
    fi
    if ! kill -0 "${pids[0]}" 2>/dev/null; then
        break
    fi
    sleep 0.05
done
if (( owner_ready == 0 )); then
    wait "${pids[0]}" || true
    sed -n '1,200p' /tmp/cxloom-token-stress-0.log
    echo "token stress owner did not become ready" >&2
    exit 1
fi

for ((host_id = 1; host_id < HOST_COUNT; ++host_id)); do
    start_host "$host_id"
    pids+=("$last_pid")
done

result=0
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    if ! wait "${pids[$host_id]}"; then
        result=1
    fi
    echo "== token stress host=${host_id} =="
    sed -n '1,200p' "/tmp/cxloom-token-stress-${host_id}.log"
done
exit "$result"
