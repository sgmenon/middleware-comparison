#!/usr/bin/env bash
# Frame-latency grid for vsomeip/results-snapshot.md (payload size ladder, fixed pace).
# Appends every measurement to bench_results/vsomeip/snapshot.csv (stable path, no temp file).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${DIR}/../.." && pwd)"
OUT="${ROOT}/vsomeip/results-snapshot.md"
SNAPSHOT_CSV="${SNAPSHOT_CSV:-${ROOT}/bench_results/vsomeip/snapshot.csv}"

# shellcheck source=size_ladder.sh
source "${DIR}/size_ladder.sh"
# Optional: SIZES="10485760" or SIZES="4194304 10485760" to append only those frames (incremental).
if [[ -n "${SIZES:-}" ]]; then
  # shellcheck disable=SC2206
  VSOMEIP_FRAME_SIZES=(${SIZES})
fi
# shellcheck source=snapshot_csv.sh
source "${DIR}/snapshot_csv.sh"

WARMUP="${WARMUP:-50}"
RUN_LABEL="${RUN_LABEL:-snapshot-$(date -u +%Y%m%dT%H%M%SZ)}"

git_sha="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
date_utc="$(date -u +"%Y-%m-%d %H:%M UTC")"

snapshot_csv_ensure "${SNAPSHOT_CSV}"

# STACKS="sgmenon" for a single stack (e.g. long 10 MiB covesa run later).
read -ra VSOMEIP_STACKS <<< "${STACKS:-covesa sgmenon}"

for SIZE in "${VSOMEIP_FRAME_SIZES[@]}"; do
  RATE_HZ="$(vsomeip_rate_for_size "${SIZE}")"
  COUNT="${COUNT:-$(vsomeip_snapshot_count_for_size "${SIZE}")}"
  for STACK in "${VSOMEIP_STACKS[@]}"; do
    echo "== ${STACK} udp frame=${SIZE} rate=${RATE_HZ} count=${COUNT} (appending to ${SNAPSHOT_CSV}) ==" >&2
    if STACK="${STACK}" SIZE="${SIZE}" RATE_HZ="${RATE_HZ}" \
      COUNT="${COUNT}" WARMUP="${WARMUP}" "${DIR}/run.sh" > /tmp/mw_vsomeip_snap.log 2>&1; then
      bench_line="$(grep -aE '^(covesa|sgmenon),' /tmp/mw_vsomeip_snap.log | tail -1 || true)"
      if [[ -n "${bench_line}" ]]; then
        snapshot_csv_append_line "${SNAPSHOT_CSV}" "${date_utc}" "${git_sha}" "${RUN_LABEL}" \
          "${COUNT}" "${WARMUP}" "${bench_line}"
      else
        snapshot_csv_append_na "${SNAPSHOT_CSV}" "${date_utc}" "${git_sha}" "${RUN_LABEL}" \
          "${COUNT}" "${WARMUP}" "${STACK}" "${SIZE}" "${RATE_HZ}"
        echo "no CSV line: ${STACK} ${SIZE} (see /tmp/mw_vsomeip_snap.log)" >&2
      fi
    else
      snapshot_csv_append_na "${SNAPSHOT_CSV}" "${date_utc}" "${git_sha}" "${RUN_LABEL}" \
        "${COUNT}" "${WARMUP}" "${STACK}" "${SIZE}" "${RATE_HZ}"
      echo "failed: ${STACK} ${SIZE} (see /tmp/mw_vsomeip_snap.log)" >&2
    fi
  done
done

render_count="${COUNT:-200}"
render_args=(
  --csv "${SNAPSHOT_CSV}"
  --out "${OUT}"
  --template "${DIR}/results-snapshot.md.jinja"
  --git-sha "${git_sha}"
  --date-utc "${date_utc}"
  --count "${render_count}"
  --warmup "${WARMUP}"
  --history-csv "${SNAPSHOT_CSV}"
)
# Partial size runs append to CSV; snapshot page uses newest row per (stack, size, rate).
if [[ -z "${SIZES:-}" ]]; then
  render_args+=(--run-label "${RUN_LABEL}")
fi
python3 "${DIR}/render_snapshot.py" "${render_args[@]}"

echo "Wrote ${OUT}"
echo "Appended run '${RUN_LABEL}' to ${SNAPSHOT_CSV}"
