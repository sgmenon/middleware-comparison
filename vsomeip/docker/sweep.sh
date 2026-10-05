#!/usr/bin/env bash
# Full size ladder at paced publish rates (no rate sweep — see vsomeip/notes.md).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${DIR}/../.." && pwd)"
OUT="${ROOT}/bench_results/vsomeip"
mkdir -p "${OUT}"

STACK="${STACK:-sgmenon}"
TRANSPORT="${TRANSPORT:-udp}"
case "${TRANSPORT}" in
  udp|tcp) ;;
  *)
    echo "TRANSPORT must be udp or tcp (got ${TRANSPORT})" >&2
    exit 2
    ;;
esac
export REPO_ROOT="${REPO_ROOT:-${ROOT}}"
export HOST_UID="${HOST_UID:-$(id -u)}"
export HOST_GID="${HOST_GID:-$(id -g)}"
export BENCH_USER="${BENCH_USER:-${USER:-bench}}"
export CONTAINER_HOME="${CONTAINER_HOME:-/home/bench}"
export BAZEL_HOST_CACHE="${BAZEL_HOST_CACHE:-${HOME}/.cache}"
export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-${HOME}/.cache/bazel-output}"

# shellcheck source=size_ladder.sh
source "${DIR}/size_ladder.sh"

CSV="${OUT}/${STACK}_${TRANSPORT}_$(date +%Y%m%d_%H%M%S).csv"
echo "stack,size,rate_hz,n,mean_us,p50_us,p99_us,gap_count" > "${CSV}"

# shellcheck source=bazel_prebuild.sh
source "${DIR}/bazel_prebuild.sh"
vsomeip_bazel_prebuild "${ROOT}"

for SIZE in "${VSOMEIP_FRAME_SIZES[@]}"; do
  RATE_HZ="$(vsomeip_rate_for_size "${SIZE}")"
  echo "== ${STACK} ${TRANSPORT} frame=${SIZE} rate=${RATE_HZ} =="
  if ! STACK="${STACK}" TRANSPORT="${TRANSPORT}" SIZE="${SIZE}" RATE_HZ="${RATE_HZ}" COUNT=2000 WARMUP=100 \
    "${DIR}/run.sh" > /tmp/mw_vsomeip_sweep.log 2>&1; then
    echo "run failed (see /tmp/mw_vsomeip_sweep.log)" >&2
    continue
  fi
  grep -aE '^(covesa|sgmenon),' /tmp/mw_vsomeip_sweep.log >> "${CSV}" || true
done

echo "Wrote ${CSV}"
