#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${DIR}/../.." && pwd)"
OUT="${ROOT}/bench_results/vsomeip"
mkdir -p "${OUT}"

STACK="${STACK:-sgmenon}"
TRANSPORT="${TRANSPORT:-udp}"
export REPO_ROOT="${REPO_ROOT:-${ROOT}}"
export BAZEL_HOST_CACHE="${BAZEL_HOST_CACHE:-${HOME}/.cache}"
export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-${HOME}/.cache/bazel-output}"
SIZES=(4096 16384 65536)
RATES=(100 500 1000 2000 5000)

CSV="${OUT}/${STACK}_${TRANSPORT}_$(date +%Y%m%d_%H%M%S).csv"
echo "stack,transport,size,rate_hz,n,mean_us,p50_us,p99_us,gap_count" > "${CSV}"

# shellcheck source=bazel_prebuild.sh
source "${DIR}/bazel_prebuild.sh"
vsomeip_bazel_prebuild "${ROOT}"

for SIZE in "${SIZES[@]}"; do
  for RATE_HZ in "${RATES[@]}"; do
    echo "== ${STACK} ${TRANSPORT} size=${SIZE} rate=${RATE_HZ} =="
    if ! STACK="${STACK}" TRANSPORT="${TRANSPORT}" SIZE="${SIZE}" RATE_HZ="${RATE_HZ}" COUNT=2000 WARMUP=100 \
      "${DIR}/run.sh" > /tmp/mw_vsomeip_sweep.log 2>&1; then
      echo "run failed (see /tmp/mw_vsomeip_sweep.log)" >&2
      continue
    fi
    grep -E '^(covesa|sgmenon),' /tmp/mw_vsomeip_sweep.log >> "${CSV}" || true
  done
done

echo "Wrote ${CSV}"
