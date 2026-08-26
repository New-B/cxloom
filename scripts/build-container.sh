#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="${CXLOOM_IMAGE:-cxloom:dev}"

docker build --tag "${IMAGE_NAME}" --file "${ROOT_DIR}/docker/Dockerfile" "${ROOT_DIR}"
