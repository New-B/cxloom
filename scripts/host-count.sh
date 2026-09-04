#!/usr/bin/env bash

cxloom_resolve_host_count() {
    local explicit="${1:-}"
    if [[ -n "$explicit" ]]; then
        echo "$explicit"
        return
    fi
    if [[ -n "${CL_HOST_COUNT:-}" ]]; then
        echo "$CL_HOST_COUNT"
        return
    fi
    local configured=""
    configured="$(docker inspect --format '{{range .Config.Env}}{{println .}}{{end}}' cxloom-h0 2>/dev/null |
        awk -F= '$1 == "CL_HOST_COUNT" { print $2; exit }')" || true
    echo "$configured"
}

cxloom_validate_host_count() {
    local count="$1"
    local minimum="${2:-1}"
    if ! [[ "$count" =~ ^[0-9]+$ ]] || (( count < minimum || count > 64 )); then
        echo "host-count must be an integer from ${minimum} to 64" >&2
        return 1
    fi
}
