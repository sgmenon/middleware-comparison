#!/usr/bin/env bash
# Payload sizes aligned with notes/benchmarks.md (ReliablePingPong SHM table).
# Each SIZE is one logical frame (reassembled event payload), not per datagram.
set -euo pipefail

# shellcheck disable=SC2034
VSOMEIP_FRAME_SIZES=(64 1024 16384 65536 262144 1048576 4194304)

# Paced publish rate (Hz): keeps one-way frame latency meaningful; not swept for CPU study.
vsomeip_rate_for_size() {
  local size="$1"
  case "${size}" in
    4194304|1048576) echo 10 ;;
    262144|65536) echo 50 ;;
    *) echo 100 ;;
  esac
}
