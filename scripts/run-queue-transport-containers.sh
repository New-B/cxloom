#!/usr/bin/env bash
set -euo pipefail

readonly HOST_COUNT=4
ITERATIONS="${CL_QUEUE_ITERATIONS:-100000}"
BATCH_SIZE="${CL_QUEUE_BATCH_SIZE:-32}"

if ! [[ "$ITERATIONS" =~ ^[1-9][0-9]*$ ]] || ! [[ "$BATCH_SIZE" =~ ^[1-9][0-9]*$ ]]; then
    echo "CL_QUEUE_ITERATIONS and CL_QUEUE_BATCH_SIZE must be positive integers" >&2
    exit 1
fi

build_pids=()
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    (
        docker exec "cxloom-h${host_id}" bash -lc '
            cmake -S /workspace -B /tmp/cxloom-build -G Ninja
            cmake --build /tmp/cxloom-build --target cxloom_queue_transport --parallel
        '
    ) >"/tmp/cxloom-queue-build-${host_id}.log" 2>&1 &
    build_pids+=("$!")
done
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    if ! wait "${build_pids[$host_id]}"; then
        sed -n '1,200p' "/tmp/cxloom-queue-build-${host_id}.log"
        exit 1
    fi
done

start_host() {
    local host_id="$1"
    docker exec --env "CL_HOST_COUNT=${HOST_COUNT}"         --env "CL_QUEUE_ITERATIONS=${ITERATIONS}"         --env "CL_QUEUE_BATCH_SIZE=${BATCH_SIZE}"         "cxloom-h${host_id}" /tmp/cxloom-build/cxloom_queue_transport         >"/tmp/cxloom-queue-transport-${host_id}.log" 2>&1 &
    last_pid=$!
}

pids=()
start_host 0
pids+=("$last_pid")

owner_ready=0
for _ in {1..600}; do
    if grep -q 'initialized shared CXL region' /tmp/cxloom-queue-transport-0.log; then
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
    sed -n '1,200p' /tmp/cxloom-queue-transport-0.log
    echo "queue transport owner did not become ready" >&2
    exit 1
fi

for host_id in 1 2 3; do
    start_host "$host_id"
    pids+=("$last_pid")
done

result=0
for ((host_id = 0; host_id < HOST_COUNT; ++host_id)); do
    if ! wait "${pids[$host_id]}"; then
        result=1
    fi
    echo "== queue transport host=${host_id} =="
    sed -n '1,200p' "/tmp/cxloom-queue-transport-${host_id}.log"
done
exit "$result"
