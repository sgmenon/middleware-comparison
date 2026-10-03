#!/usr/bin/env bash
# Append vsomeip bench rows to a stable history CSV (see run_snapshot.sh).
set -euo pipefail

SNAPSHOT_CSV_HEADER="recorded_utc,git_sha,run_label,count,warmup,stack,size,rate_hz,n,mean_us,p50_us,p99_us,gap_count"

snapshot_csv_ensure() {
  local path="$1"
  mkdir -p "$(dirname "${path}")"
  if [[ ! -f "${path}" ]]; then
    echo "${SNAPSHOT_CSV_HEADER}" > "${path}"
  fi
}

# Args: path, recorded_utc, git_sha, run_label, count, warmup, bench_csv_line (8 fields)
snapshot_csv_append_line() {
  local path="$1" recorded="$2" sha="$3" label="$4" count="$5" warmup="$6" bench_line="$7"
  echo "${recorded},${sha},${label},${count},${warmup},${bench_line}" >> "${path}"
}

snapshot_csv_append_na() {
  local path="$1" recorded="$2" sha="$3" label="$4" count="$5" warmup="$6"
  local stack="$7" size="$8" rate_hz="$9"
  snapshot_csv_append_line "${path}" "${recorded}" "${sha}" "${label}" "${count}" "${warmup}" \
    "${stack},${size},${rate_hz},0,NA,NA,NA,NA"
}
