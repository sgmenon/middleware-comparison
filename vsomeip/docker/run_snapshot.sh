#!/usr/bin/env bash
# Frame-latency grid for vsomeip/results-snapshot.md (payload size ladder, fixed pace).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${DIR}/../.." && pwd)"
OUT="${ROOT}/vsomeip/results-snapshot.md"
CSV_TMP="$(mktemp)"

# shellcheck source=size_ladder.sh
source "${DIR}/size_ladder.sh"

COUNT="${COUNT:-200}"
WARMUP="${WARMUP:-50}"

echo "stack,size,rate_hz,n,mean_us,p50_us,p99_us,gap_count" > "${CSV_TMP}"

for SIZE in "${VSOMEIP_FRAME_SIZES[@]}"; do
  RATE_HZ="$(vsomeip_rate_for_size "${SIZE}")"
  for STACK in covesa sgmenon; do
    echo "== ${STACK} udp frame=${SIZE} rate=${RATE_HZ} ==" >&2
    if STACK="${STACK}" SIZE="${SIZE}" RATE_HZ="${RATE_HZ}" \
      COUNT="${COUNT}" WARMUP="${WARMUP}" "${DIR}/run.sh" > /tmp/mw_vsomeip_snap.log 2>&1; then
      if ! grep -aE '^(covesa|sgmenon),' /tmp/mw_vsomeip_snap.log >> "${CSV_TMP}"; then
        echo "${STACK},${SIZE},${RATE_HZ},0,NA,NA,NA,NA" >> "${CSV_TMP}"
        echo "no CSV line: ${STACK} ${SIZE} (see /tmp/mw_vsomeip_snap.log)" >&2
      fi
    else
      echo "${STACK},${SIZE},${RATE_HZ},0,NA,NA,NA,NA" >> "${CSV_TMP}"
      echo "failed: ${STACK} ${SIZE} (see /tmp/mw_vsomeip_snap.log)" >&2
    fi
  done
done

git_sha="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
date_utc="$(date -u +"%Y-%m-%d %H:%M UTC")"

python3 "${DIR}/render_snapshot.py" \
  --csv "${CSV_TMP}" \
  --out "${OUT}" \
  --template "${DIR}/results-snapshot.md.jinja" \
  --git-sha "${git_sha}" \
  --date-utc "${date_utc}" \
  --count "${COUNT}" \
  --warmup "${WARMUP}"

rm -f "${CSV_TMP}"
echo "Wrote ${OUT}"
