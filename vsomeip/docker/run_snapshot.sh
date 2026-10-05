#!/usr/bin/env bash
# Frame-latency grid for vsomeip/results-snapshot.md (payload size ladder, fixed pace).
# Appends every measurement to bench_results/vsomeip/snapshot.csv (stable path, no temp file).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${DIR}/../.." && pwd)"
TRANSPORT="${TRANSPORT:-udp}"
OUT="${OUT:-${ROOT}/vsomeip/results-snapshot.md}"
UDP_CSV="${UDP_CSV:-${ROOT}/bench_results/vsomeip/snapshot.csv}"
TCP_CSV="${TCP_CSV:-${ROOT}/bench_results/vsomeip/snapshot-tcp.csv}"
case "${TRANSPORT}" in
  udp)
    SNAPSHOT_CSV="${SNAPSHOT_CSV:-${UDP_CSV}}"
    ;;
  tcp)
    SNAPSHOT_CSV="${SNAPSHOT_CSV:-${TCP_CSV}}"
    ;;
  *)
    echo "TRANSPORT must be udp or tcp (got ${TRANSPORT})" >&2
    exit 2
    ;;
esac
RENDER_ONLY="${1:-0}"
git_sha="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
date_utc="$(date -u +"%Y-%m-%d %H:%M UTC")"
WARMUP="${WARMUP:-50}"
if [[ -z "${RUN_LABEL:-}" && "${RENDER_ONLY}" != "0" && -s "${SNAPSHOT_CSV}" ]]; then
  # Render-only: show the most recent run in the CSV.
  RUN_LABEL="$(tail -n +2 "${SNAPSHOT_CSV}" | tail -1 | cut -d, -f3)"
fi
RUN_LABEL="${RUN_LABEL:-snapshot-$(date -u +%Y%m%dT%H%M%SZ)}"
# Per-run docker logs: <stack>_<transport>_<size>.log
LOG_DIR="${LOG_DIR:-${ROOT}/bench_results/vsomeip/logs/${RUN_LABEL}}"

# shellcheck source=size_ladder.sh
if [[ "${RENDER_ONLY:-0}" == "0" ]]; then
  source "${DIR}/size_ladder.sh"
  # Optional: SIZES="10485760" or SIZES="4194304 10485760" to append only those frames (incremental).
  if [[ -n "${SIZES:-}" ]]; then
    # shellcheck disable=SC2206
    VSOMEIP_FRAME_SIZES=(${SIZES})
  fi
  # shellcheck source=snapshot_csv.sh
  source "${DIR}/snapshot_csv.sh"

  snapshot_csv_ensure "${SNAPSHOT_CSV}"
  mkdir -p "${LOG_DIR}"

  # STACKS="sgmenon" for a single stack (e.g. long 10 MiB covesa run later).
  read -ra VSOMEIP_STACKS <<< "${STACKS:-covesa sgmenon}"

  for SIZE in "${VSOMEIP_FRAME_SIZES[@]}"; do
    RATE_HZ="$(vsomeip_rate_for_size "${SIZE}")"
    run_count="${COUNT:-$(vsomeip_snapshot_count_for_size "${SIZE}")}"
    for STACK in "${VSOMEIP_STACKS[@]}"; do
      log="${LOG_DIR}/${STACK}_${TRANSPORT}_${SIZE}.log"
      echo "== ${STACK} ${TRANSPORT} frame=${SIZE} rate=${RATE_HZ} count=${run_count} (log: ${log}) ==" >&2
      if STACK="${STACK}" TRANSPORT="${TRANSPORT}" SIZE="${SIZE}" RATE_HZ="${RATE_HZ}" \
        COUNT="${run_count}" WARMUP="${WARMUP}" "${DIR}/run.sh" > "${log}" 2>&1; then
        bench_line="$(grep -aE '^(covesa|sgmenon),' "${log}" | tail -1 || true)"
        if [[ -n "${bench_line}" ]]; then
          snapshot_csv_append_line "${SNAPSHOT_CSV}" "${date_utc}" "${git_sha}" "${RUN_LABEL}" \
            "${run_count}" "${WARMUP}" "${bench_line}"
        else
          snapshot_csv_append_na "${SNAPSHOT_CSV}" "${date_utc}" "${git_sha}" "${RUN_LABEL}" \
            "${run_count}" "${WARMUP}" "${STACK}" "${SIZE}" "${RATE_HZ}"
          echo "no CSV line: ${STACK} ${SIZE} (see ${log})" >&2
        fi
      else
        snapshot_csv_append_na "${SNAPSHOT_CSV}" "${date_utc}" "${git_sha}" "${RUN_LABEL}" \
          "${run_count}" "${WARMUP}" "${STACK}" "${SIZE}" "${RATE_HZ}"
        echo "failed: ${STACK} ${SIZE} (see ${log})" >&2
      fi
    done
  done
  echo "Appended run '${RUN_LABEL}' to ${SNAPSHOT_CSV}"
  echo "Logs: ${LOG_DIR}"
fi

latest_run_label() {
  local csv="$1"
  if [[ -s "${csv}" ]]; then
    tail -n +2 "${csv}" | tail -1 | cut -d, -f3
  fi
}

udp_run_label="$(latest_run_label "${UDP_CSV}")"
tcp_run_label="$(latest_run_label "${TCP_CSV}")"
# A full run renders that exact run. Partial size runs retain latest-per-config behavior.
if [[ -z "${SIZES:-}" ]]; then
  if [[ "${TRANSPORT}" == "udp" ]]; then
    udp_run_label="${RUN_LABEL}"
  else
    tcp_run_label="${RUN_LABEL}"
  fi
elif [[ "${TRANSPORT}" == "udp" ]]; then
  udp_run_label=""
else
  tcp_run_label=""
fi

render_args=(
  --udp-csv "${UDP_CSV}"
  --tcp-csv "${TCP_CSV}"
  --out "${OUT}"
  --template "${DIR}/results-snapshot.md.jinja"
  --git-sha "${git_sha}"
  --date-utc "${date_utc}"
  --warmup "${WARMUP}"
)
[[ -n "${udp_run_label}" ]] && render_args+=(--udp-run-label "${udp_run_label}")
[[ -n "${tcp_run_label}" ]] && render_args+=(--tcp-run-label "${tcp_run_label}")
python3 "${DIR}/render_snapshot.py" "${render_args[@]}"
echo "Wrote ${OUT} (run '${RUN_LABEL}')"
