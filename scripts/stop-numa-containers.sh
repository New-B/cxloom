#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/stop-numa-containers.sh
# Stop and remove all containers with names starting with 'cxloom-h'.
# No arguments required.

set +e
names=$(docker ps -a --format '{{.Names}}' 2>/dev/null | grep -E '^cxloom-h' || true)
set -e

if [[ -z "${names}" ]]; then
    echo "No containers found with prefix 'cxloom-h'."
    exit 0
fi

while IFS= read -r container_name; do
    if [[ -n "$container_name" ]]; then
        echo "Removing ${container_name}"
        docker rm --force "$container_name" || true
    fi
done <<< "${names}"
