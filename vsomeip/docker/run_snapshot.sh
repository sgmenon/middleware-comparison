#!/usr/bin/env bash
# Small grid for vsomeip/results-snapshot.md (not a full rate sweep).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${DIR}/../.." && pwd)"
OUT="${ROOT}/vsomeip/results-snapshot.md"
CSV_TMP="$(mktemp)"

COUNT="${COUNT:-500}"
WARMUP="${WARMUP:-50}"

echo "stack,transport,size,rate_hz,n,mean_us,p50_us,p99_us,gap_count" > "${CSV_TMP}"

runs=(
  "covesa tcp 4096 500"
  "covesa tcp 4096 1000"
  "covesa udp 4096 500"
  "covesa udp 65536 500"
  "sgmenon tcp 4096 500"
  "sgmenon tcp 4096 1000"
  "sgmenon udp 4096 500"
  "sgmenon udp 65536 500"
)

for spec in "${runs[@]}"; do
  read -r STACK TRANSPORT SIZE RATE_HZ <<< "${spec}"
  echo "== ${spec} ==" >&2
  if STACK="${STACK}" TRANSPORT="${TRANSPORT}" SIZE="${SIZE}" RATE_HZ="${RATE_HZ}" \
    COUNT="${COUNT}" WARMUP="${WARMUP}" "${DIR}/run.sh" > /tmp/mw_vsomeip_snap.log 2>&1; then
    grep -E '^(covesa|sgmenon),' /tmp/mw_vsomeip_snap.log >> "${CSV_TMP}" || true
  else
    echo "${STACK},${TRANSPORT},${SIZE},${RATE_HZ},0,NA,NA,NA,NA" >> "${CSV_TMP}"
    echo "failed: ${spec} (see /tmp/mw_vsomeip_snap.log)" >&2
  fi
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
