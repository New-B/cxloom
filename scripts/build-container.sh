#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="${CXLOOM_IMAGE:-cxloom:dev}"

# Enable BuildKit by default (allow override) and forward proxy build-args
DOCKER_BUILDKIT="${DOCKER_BUILDKIT:-1}"
export DOCKER_BUILDKIT

# Collect build-args for proxy if provided in the environment
BUILD_ARGS=()
if [ -n "${HTTP_PROXY:-}" ] || [ -n "${http_proxy:-}" ]; then
	# prefer explicit HTTPS_PROXY, fallback to HTTP_PROXY
	HTTP_ARG="${HTTP_PROXY:-${http_proxy:-}}"
	HTTPS_ARG="${HTTPS_PROXY:-${https_proxy:-$HTTP_ARG}}"
	BUILD_ARGS+=(--build-arg "HTTP_PROXY=${HTTP_ARG}")
	BUILD_ARGS+=(--build-arg "HTTPS_PROXY=${HTTPS_ARG}")
fi

# Optionally disable cache by setting NO_CACHE=1 in the environment
NO_CACHE_FLAG=""
if [ "${NO_CACHE:-}" = "1" ] || [ "${NO_CACHE:-}" = "true" ]; then
	NO_CACHE_FLAG="--no-cache"
fi

docker build ${NO_CACHE_FLAG} --progress=plain --network=host --tag "${IMAGE_NAME}" --file "${ROOT_DIR}/docker/Dockerfile" "${ROOT_DIR}" "${BUILD_ARGS[@]}"
