#!/usr/bin/env bash
# Build opt net CLIs on the host, stage into the image, run 2-container bridge.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STACK="${STACK:-subspace}"
SIZE="${SIZE:-1024}"
COUNT="${COUNT:-1000}"

case "${STACK}" in
  subspace|zenoh|cyclone) ;;
  *)
    echo "STACK must be subspace|zenoh|cyclone (got ${STACK})" >&2
    exit 2
    ;;
esac

# Default peer endpoints for the pub container.
if [[ -z "${PEER:-}" ]]; then
  case "${STACK}" in
    subspace) PEER=sub:7420 ;;
    zenoh) PEER=sub:7447 ;;
    cyclone) PEER=sub:7400 ;;
  esac
fi
export PEER STACK SIZE COUNT

echo "== bazel build --config=opt net CLIs =="
(
  cd "${ROOT}"
  bazel build --config=opt \
    //benchmarks/net:subspace_net \
    //benchmarks/net:zenoh_net \
    //benchmarks/net:cyclone_net
)

BIN_ROOT="${ROOT}/bazel-bin"
STAGE="${DIR}/staging"
rm -rf "${STAGE}"
mkdir -p "${STAGE}/benchmarks/net"

echo "== stage binaries + runfiles + _solib_local =="
for name in subspace_net zenoh_net cyclone_net; do
  cp -a "${BIN_ROOT}/benchmarks/net/${name}" "${STAGE}/benchmarks/net/"
  if [[ -d "${BIN_ROOT}/benchmarks/net/${name}.runfiles" ]]; then
    # Follow Bazel absolute symlinks so libs exist inside the image.
    rsync -aL --delete "${BIN_ROOT}/benchmarks/net/${name}.runfiles/" "${STAGE}/benchmarks/net/${name}.runfiles/"
  fi
done
if [[ -d "${BIN_ROOT}/_solib_local" ]]; then
  rsync -aL --delete "${BIN_ROOT}/_solib_local/" "${STAGE}/_solib_local/"
fi

echo "== docker compose build + run (STACK=${STACK} SIZE=${SIZE} COUNT=${COUNT}) =="
cd "${DIR}"
docker compose --profile net build
# Start sub, then pub; abort when pub finishes; scrape pub CSV from logs.
docker compose --profile net up --abort-on-container-exit --exit-code-from pub 2>&1 | tee /tmp/mw_net_compose.log
# Print the CSV summary line(s) from pub.
echo "== result =="
# Compose prefixes lines as "mw_net_pub  | csv..."
docker compose --profile net logs pub 2>/dev/null | sed -n 's/.*[[:space:]]| //p' | grep -E '^(subspace|zenoh|cyclone),' || \
  sed -n 's/.*[[:space:]]| //p' /tmp/mw_net_compose.log | grep -E '^(subspace|zenoh|cyclone),' || \
  grep -E '^(subspace|zenoh|cyclone),' /tmp/mw_net_compose.log || true

docker compose --profile net down --remove-orphans >/dev/null 2>&1 || true
