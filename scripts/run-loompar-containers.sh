#!/usr/bin/env bash
set -euo pipefail
# Acceptance always uses cxloom-h0 through cxloom-h15 and their shared CL_DAX_DEVICE.
# Run exclusively: host 0 formats the configured shared region.
log_dir="$(mktemp -d /tmp/cxloom-loompar-containers.XXXXXX)"
pids=()
program="${CL_PAR_PROGRAM:-cxloom_loompar_threads}"
case "$program" in
    cxloom_loompar_threads|cxloom_loompar_sync) ;;
    *) echo "CL_PAR_PROGRAM must select cxloom_loompar_threads or cxloom_loompar_sync" >&2; exit 2 ;;
esac
rounds="${CL_PAR_ROUNDS:-300}"
creators="${CL_PAR_CREATORS:-4}"
delay_ms="${CL_PAR_ROUND_DELAY_MS:-1000}"
run_timeout="${CL_PAR_TIMEOUT_SECONDS:-1800}"
for value in "$rounds" "$creators" "$run_timeout"; do
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "rounds, creators and timeout must be positive integers" >&2; exit 2; }
done
[[ "$delay_ms" =~ ^[0-9]+$ ]] || { echo "delay must be a nonnegative integer" >&2; exit 2; }
echo "16-container acceptance: program=${program}, rounds=${rounds}, creators=${creators}, delay_ms=${delay_ms}, logs=${log_dir}"
# Check every participant before host 0 is allowed to format the shared region.
for ((host = 0; host < 16; ++host)); do
    running="$(docker inspect --format '{{.State.Running}}' "cxloom-h${host}")" || exit 1
    [[ "$running" == true ]] || { echo "cxloom-h${host} is not running" >&2; exit 1; }
done
for ((host = 0; host < 16; ++host)); do
    docker exec --env "CL_PAR_PROGRAM=${program}" "cxloom-h${host}" bash -lc '
        cmake -S /workspace -B /tmp/cxloom-build -G Ninja
        cmake --build /tmp/cxloom-build --target "$CL_PAR_PROGRAM" --parallel
    ' >"${log_dir}/build-${host}.log" 2>&1 || { cat "${log_dir}/build-${host}.log"; exit 1; }
done
start_host() {
    docker exec --env "CL_HOST_ID=$1" --env CL_HOST_COUNT=16 \
        --env "CL_PAR_ROUNDS=${rounds}" --env "CL_PAR_CREATORS=${creators}" \
        --env "CL_PAR_ROUND_DELAY_MS=${delay_ms}" "cxloom-h$1" \
        timeout "$run_timeout" "/tmp/cxloom-build/${program}" >"${log_dir}/host-$1.log" 2>&1 &
    pids+=("$!")
}
start_host 0
ready=0
for ((attempt = 0; attempt < 400; ++attempt)); do
    if rg -q 'region ready' "${log_dir}/host-0.log"; then ready=1; break; fi
    if ! kill -0 "${pids[0]}" 2>/dev/null; then break; fi
    sleep 0.05
done
if ((ready == 0)); then
    cat "${log_dir}/host-0.log"
    echo "owner did not initialize; logs: ${log_dir}" >&2
    exit 1
fi
for ((host = 1; host < 16; ++host)); do start_host "$host"; done
result=0
for pid in "${pids[@]}"; do wait "$pid" || result=1; done
for ((host = 0; host < 16; ++host)); do
    cat "${log_dir}/host-${host}.log"
    rg -q "^PASS host=${host} hosts=16 " "${log_dir}/host-${host}.log" || result=1
done
echo "logs: ${log_dir}"
exit "$result"
