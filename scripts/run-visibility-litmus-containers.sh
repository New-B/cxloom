#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/host-count.sh"
CONTAINER_COUNT="$(cxloom_resolve_host_count "${1:-}")"
VISIBILITY_MODES="${CL_VISIBILITY_MODES:-release seq_cst clflush clwb}"
ITERATIONS="${CL_VISIBILITY_ITERATIONS:-1000}"

if ! [[ "$CONTAINER_COUNT" =~ ^[1-9][0-9]*$ ]] || (( CONTAINER_COUNT > 64 )); then
    echo "container-count must be an integer from 1 to 64" >&2
    exit 1
fi
if ! [[ "$ITERATIONS" =~ ^[1-9][0-9]*$ ]]; then
    echo "CL_VISIBILITY_ITERATIONS must be a positive integer" >&2
    exit 1
fi

build_pids=()
for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    (
        docker exec "cxloom-h${host_id}" bash -lc '
            cmake -S /workspace -B /tmp/cxloom-build -G Ninja
            cmake --build /tmp/cxloom-build --target cxloom_visibility_litmus --parallel
        '
    ) >"/tmp/cxloom-litmus-build-${host_id}.log" 2>&1 &
    build_pids+=("$!")
done
for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
    if ! wait "${build_pids[$host_id]}"; then
        sed -n '1,200p' "/tmp/cxloom-litmus-build-${host_id}.log"
        exit 1
    fi
done

run_mode() {
    local mode="$1"
    local pids=()
    local log_prefix="/tmp/cxloom-litmus-${mode}"

    docker exec --env "CL_HOST_COUNT=${CONTAINER_COUNT}" --env "CL_VISIBILITY_MODE=${mode}"         --env "CL_VISIBILITY_ITERATIONS=${ITERATIONS}"         cxloom-h0 /tmp/cxloom-build/cxloom_visibility_litmus         >"${log_prefix}-0.log" 2>&1 &
    pids+=("$!")

    local owner_ready=0
    for _ in {1..600}; do
        if grep -q 'initialized shared CXL region' "${log_prefix}-0.log"; then
            owner_ready=1
            break
        fi
        if ! kill -0 "${pids[0]}" 2>/dev/null; then break; fi
        sleep 0.05
    done
    if (( owner_ready == 0 )); then
        wait "${pids[0]}" || true
        sed -n '1,200p' "${log_prefix}-0.log"
        echo "litmus owner did not become ready for mode ${mode}" >&2
        return 1
    fi

    for ((host_id = 1; host_id < CONTAINER_COUNT; ++host_id)); do
        docker exec --env "CL_HOST_COUNT=${CONTAINER_COUNT}" --env "CL_VISIBILITY_MODE=${mode}"             --env "CL_VISIBILITY_ITERATIONS=${ITERATIONS}"             "cxloom-h${host_id}" /tmp/cxloom-build/cxloom_visibility_litmus             >"${log_prefix}-${host_id}.log" 2>&1 &
        pids+=("$!")
    done

    local result=0
    for ((host_id = 0; host_id < CONTAINER_COUNT; ++host_id)); do
        if ! wait "${pids[$host_id]}"; then result=1; fi
        echo "== mode=${mode} host=${host_id} =="
        sed -n '1,200p' "${log_prefix}-${host_id}.log"
    done
    return "$result"
}

result=0
passed_modes=""
failed_modes=""
for mode in $VISIBILITY_MODES; do
    echo "==== visibility mode: ${mode} ===="
    if run_mode "$mode"; then
        passed_modes="$passed_modes $mode"
    else
        failed_modes="$failed_modes $mode"
        result=1
    fi
done
echo "==== visibility summary ===="
echo "passed:${passed_modes:- none}"
echo "failed:${failed_modes:- none}"
if (( result == 0 )); then
    set -- $passed_modes
    echo "weakest zero-error candidate in requested order: $1"
fi
exit "$result"
