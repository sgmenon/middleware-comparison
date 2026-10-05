#!/usr/bin/env bash
# Payload sizes aligned with notes/benchmarks.md (ReliablePingPong SHM table).
# Each SIZE is one logical frame, not one UDP chunk or TCP segment.
set -euo pipefail

# shellcheck disable=SC2034
VSOMEIP_FRAME_SIZES=(64 1024 16384 65536 262144 1048576 4194304 10485760)

# Paced publish rate (Hz): keeps one-way frame latency meaningful; not swept for CPU study.
vsomeip_rate_for_size() {
  local size="$1"
  case "${size}" in
    # Period must exceed the slower stack's per-frame time, or latency measures queue backlog.
    10485760) echo 1 ;;
    4194304) echo 2 ;;
    1048576) echo 10 ;;
    262144|65536) echo 50 ;;
    *) echo 100 ;;
  esac
}

# Default sample count for snapshot grid (override with COUNT=…).
vsomeip_snapshot_count_for_size() {
  local size="$1"
  case "${size}" in
    10485760) echo 50 ;;
    4194304) echo 100 ;;
    *) echo 200 ;;
  esac
}
