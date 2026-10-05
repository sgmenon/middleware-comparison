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
RPC_CALLS="${RPC_CALLS:-0}"
TRANSPORT="${TRANSPORT:-udp}"

case "${STACK}" in
  covesa|sgmenon) ;;
  *)
    echo "STACK must be covesa (baseline) or sgmenon (improved) (got ${STACK})" >&2
    exit 2
    ;;
esac
case "${TRANSPORT}" in
  udp|tcp) ;;
  *)
    echo "TRANSPORT must be udp or tcp (got ${TRANSPORT})" >&2
    exit 2
    ;;
esac

export STACK SIZE COUNT RATE_HZ WARMUP MAX_DATAGRAM RPC_CALLS TRANSPORT
export REPO_ROOT="${ROOT}"
export HOST_UID="${HOST_UID:-$(id -u)}"
export HOST_GID="${HOST_GID:-$(id -g)}"
export BENCH_USER="${BENCH_USER:-${USER:-bench}}"
export BAZEL_HOST_CACHE="${BAZEL_HOST_CACHE:-${HOME}/.cache}"
export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-${HOME}/.cache/bazel-output}"
export CONTAINER_HOME="${CONTAINER_HOME:-/home/bench}"

# One compose stack at a time (fixed container names + shared Bazel output in containers).
LOCK_FILE="${MW_VSOMEIP_LOCK:-/tmp/mw_vsomeip_compose.lock}"
exec 9>"${LOCK_FILE}"
if ! flock -n 9; then
  echo "Another vsomeip bench holds ${LOCK_FILE} (snapshot, profile, or stale run)." >&2
  echo "Wait for it to finish, or: cd vsomeip/docker && docker compose --profile vsomeip down" >&2
  exit 3
fi

# shellcheck source=bazel_prebuild.sh
source "${DIR}/bazel_prebuild.sh"
vsomeip_bazel_prebuild "${ROOT}"

echo "== docker compose (STACK=${STACK} ${TRANSPORT}) =="
cd "${DIR}"
docker compose --profile vsomeip down --remove-orphans >/dev/null 2>&1 || true
docker compose --profile vsomeip build
# Do not use --abort-on-container-exit: pub finishes the notify loop before sub collects COUNT samples.
docker compose --profile vsomeip up --exit-code-from sub 2>&1 | tee /tmp/mw_vsomeip_compose.log

echo "== result =="
if [[ "${RPC_CALLS}" -gt 0 ]]; then
  sed -n 's/.*[[:space:]]| //p' /tmp/mw_vsomeip_compose.log | grep -E '^rpc' || true
fi
docker compose --profile vsomeip logs sub 2>/dev/null | sed -n 's/.*[[:space:]]| //p' | grep -E '^(sgmenon|covesa),' || \
  sed -n 's/.*[[:space:]]| //p' /tmp/mw_vsomeip_compose.log | grep -E '^(sgmenon|covesa),' || \
  grep -E '^(sgmenon|covesa),' /tmp/mw_vsomeip_compose.log || true

docker compose --profile vsomeip down --remove-orphans >/dev/null 2>&1 || true
