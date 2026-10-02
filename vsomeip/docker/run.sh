#!/usr/bin/env bash
# vsomeip event bench: Bazel builds and runs inside each container (repo mounted at /workspace).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

STACK="${STACK:-sgmenon}"
SIZE="${SIZE:-4096}"
COUNT="${COUNT:-1000}"
RATE_HZ="${RATE_HZ:-1000}"
WARMUP="${WARMUP:-50}"
MAX_DATAGRAM="${MAX_DATAGRAM:-1400}"

case "${STACK}" in
  covesa|sgmenon) ;;
  *)
    echo "STACK must be covesa (baseline) or sgmenon (improved) (got ${STACK})" >&2
    exit 2
    ;;
esac

export STACK SIZE COUNT RATE_HZ WARMUP MAX_DATAGRAM
export REPO_ROOT="${ROOT}"
export HOST_UID="${HOST_UID:-$(id -u)}"
export HOST_GID="${HOST_GID:-$(id -g)}"
export BENCH_USER="${BENCH_USER:-${USER:-bench}}"
export BAZEL_HOST_CACHE="${BAZEL_HOST_CACHE:-${HOME}/.cache}"
export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-${HOME}/.cache/bazel-output}"
export CONTAINER_HOME="${CONTAINER_HOME:-/home/bench}"

# shellcheck source=bazel_prebuild.sh
source "${DIR}/bazel_prebuild.sh"
vsomeip_bazel_prebuild "${ROOT}"

echo "== docker compose (STACK=${STACK} udp) =="
cd "${DIR}"
docker compose --profile vsomeip build
# Do not use --abort-on-container-exit: pub finishes the notify loop before sub collects COUNT samples.
docker compose --profile vsomeip up --exit-code-from sub 2>&1 | tee /tmp/mw_vsomeip_compose.log

echo "== result =="
docker compose --profile vsomeip logs sub 2>/dev/null | sed -n 's/.*[[:space:]]| //p' | grep -E '^(sgmenon|covesa),' || \
  sed -n 's/.*[[:space:]]| //p' /tmp/mw_vsomeip_compose.log | grep -E '^(sgmenon|covesa),' || \
  grep -E '^(sgmenon|covesa),' /tmp/mw_vsomeip_compose.log || true

docker compose --profile vsomeip down --remove-orphans >/dev/null 2>&1 || true
