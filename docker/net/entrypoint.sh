#!/usr/bin/env bash
# Container entrypoint: run STACK_net as ROLE=pub|sub.
set -euo pipefail

STACK="${STACK:-subspace}"
ROLE="${ROLE:-sub}"
SIZE="${SIZE:-1024}"
COUNT="${COUNT:-1000}"
CHANNEL="${CHANNEL:-bench/net}"
WARMUP="${WARMUP:-50}"

BIN="/opt/mw/benchmarks/net/${STACK}_net"
if [[ ! -x "${BIN}" ]]; then
  echo "missing binary: ${BIN} (STACK=${STACK})" >&2
  exit 1
fi

args=(--role="${ROLE}" --size="${SIZE}" --count="${COUNT}" --channel="${CHANNEL}" --warmup="${WARMUP}")

case "${STACK}" in
  subspace)
    if [[ "${ROLE}" == "sub" ]]; then
      args+=(--disc-port="${DISC_PORT:-7420}")
    else
      args+=(--peer="${PEER:-sub:7420}" --disc-port="${DISC_PORT:-7421}")
    fi
    ;;
  zenoh)
    if [[ "${ROLE}" == "sub" ]]; then
      args+=(--disc-port="${DISC_PORT:-7447}")
    else
      args+=(--peer="${PEER:-sub:7447}")
    fi
    ;;
  cyclone)
    if [[ "${ROLE}" == "pub" ]]; then
      args+=(--peer="${PEER:-sub:7400}")
    else
      args+=(--peer="${PEER:-pub:7400}")
    fi
    ;;
  *)
    echo "unknown STACK=${STACK}" >&2
    exit 1
    ;;
esac

echo "starting ${STACK}_net ${args[*]}" >&2
exec "${BIN}" "${args[@]}"
