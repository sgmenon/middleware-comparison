#!/usr/bin/env bash
# vsomeip event bench: Bazel builds and runs inside each container (repo mounted at /workspace).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

STACK="${STACK:-sgmenon}"
TRANSPORT="${TRANSPORT:-tcp}"
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

export STACK TRANSPORT SIZE COUNT RATE_HZ WARMUP MAX_DATAGRAM
export REPO_ROOT="${ROOT}"
export BAZEL_HOST_CACHE="${BAZEL_HOST_CACHE:-${HOME}/.cache}"
export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-${HOME}/.cache/bazel-output}"

# shellcheck source=bazel_prebuild.sh
source "${DIR}/bazel_prebuild.sh"
vsomeip_bazel_prebuild "${ROOT}"

echo "== docker compose (STACK=${STACK} TRANSPORT=${TRANSPORT}) =="
cd "${DIR}"
docker compose --profile vsomeip build
docker compose --profile vsomeip up --abort-on-container-exit --exit-code-from sub 2>&1 | tee /tmp/mw_vsomeip_compose.log

echo "== result =="
docker compose --profile vsomeip logs sub 2>/dev/null | sed -n 's/.*[[:space:]]| //p' | grep -E '^(sgmenon|covesa),' || \
  sed -n 's/.*[[:space:]]| //p' /tmp/mw_vsomeip_compose.log | grep -E '^(sgmenon|covesa),' || \
  grep -E '^(sgmenon|covesa),' /tmp/mw_vsomeip_compose.log || true

docker compose --profile vsomeip down --remove-orphans >/dev/null 2>&1 || true
