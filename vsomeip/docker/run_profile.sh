#!/usr/bin/env bash
# Tracy-instrumented vsomeip bench (build with --config=tracy_docker).
# Start Tracy profiler or tracy-capture on the host before running.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export VSOMEIP_TRACY=1
export BAZEL_CONFIG_TRACY="${BAZEL_CONFIG_TRACY:-tracy_docker}"

# Default: Docker bridge gateway on linux (override if your host IP differs).
export TRACY_CLIENT_ADDRESS="${TRACY_CLIENT_ADDRESS:-172.29.0.1}"

STACK="${STACK:-sgmenon}"
TRANSPORT="${TRANSPORT:-udp}"
SIZE="${SIZE:-64}"
COUNT="${COUNT:-30}"
WARMUP="${WARMUP:-5}"
RATE_HZ="${RATE_HZ:-100}"

echo "Tracy: on the host, start tracy-capture (or Tracy GUI) before this run, e.g.:" >&2
echo "  tracy-capture -o /tmp/vsomeip.tracy -a 0.0.0.0" >&2
echo "Instrumented clients → ${TRACY_CLIENT_ADDRESS}:8086 (pub + sub containers)" >&2
echo "Build: --config=${BAZEL_CONFIG_TRACY}  stack=${STACK} size=${SIZE} count=${COUNT}" >&2

exec env STACK="${STACK}" TRANSPORT="${TRANSPORT}" SIZE="${SIZE}" COUNT="${COUNT}" WARMUP="${WARMUP}" RATE_HZ="${RATE_HZ}" RPC_CALLS="${RPC_CALLS:-0}" \
  VSOMEIP_TRACY=1 TRACY_CLIENT_ADDRESS="${TRACY_CLIENT_ADDRESS}" \
  "${DIR}/run.sh"
