#!/usr/bin/env bash
# Bazel in-container: shared ~/.cache/bazel-output. Use build + exec (not `bazel run`) for long-lived
# bench/R routingmanagerd processes — `bazel run` keeps the output-base lock until the child exits.
set -euo pipefail

if [[ "${VSOMEIP_DOCKER_NET_SETUP:-1}" != "0" ]]; then
  /usr/local/bin/multicast_setup.sh
fi

ROLE="${ROLE:?ROLE must be service (pub) or client (sub)}"
STACK="${STACK:?STACK must be covesa (baseline) or sgmenon (improved)}"
TRANSPORT="${TRANSPORT:-tcp}"
SIZE="${SIZE:-4096}"
COUNT="${COUNT:-1000}"
RATE_HZ="${RATE_HZ:-1000}"
WARMUP="${WARMUP:-50}"
MAX_DATAGRAM="${MAX_DATAGRAM:-1400}"

CLIENT_IP="${CLIENT_IP:-172.29.0.2}"
SERVICE_IP="${SERVICE_IP:-172.29.0.3}"

WS="${WORKSPACE:-/workspace}"
export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-/root/.cache/bazel-output}"
TPL="${WS}/vsomeip/config"
CFG_DIR="/tmp/vsomeip-config"
mkdir -p "${CFG_DIR}"

case "${STACK}" in
  covesa|sgmenon) ;;
  *)
    echo "STACK must be covesa|sgmenon (got ${STACK})" >&2
    exit 2
    ;;
esac

if [[ "${ROLE}" == "service" ]]; then
  BENCH="//vsomeip/bench:event_service_${STACK}"
  HOST_IP="${SERVICE_IP}"
  UNICAST="${SERVICE_IP}"
  rm -f /tmp/vsomeip-service-ready
else
  BENCH="//vsomeip/bench:event_client_${STACK}"
  HOST_IP="${CLIENT_IP}"
  UNICAST="${CLIENT_IP}"
fi

if [[ "${STACK}" == "covesa" ]]; then
  ROUTING="routingmanagerd"
  RM_LABEL="@vsomeip_covesa//examples/routingmanagerd:routingmanagerd"
else
  ROUTING=$([[ "${ROLE}" == "service" ]] && echo bench_service || echo bench_client)
  RM_LABEL=""
fi

sed -e "s/@UNICAST@/${UNICAST}/g" -e "s/@ROUTING@/${ROUTING}/g" \
  "${TPL}/vsomeip.json.in" > "${CFG_DIR}/bench.json"
if [[ "${ROLE}" == "client" ]]; then
  # Subscriber discovers remote offers via SD (see sgmenon event_test docker configs).
  python3 - <<'PY'
import json
from pathlib import Path

path = Path("/tmp/vsomeip-config/bench.json")
cfg = json.loads(path.read_text())
cfg.pop("services", None)
path.write_text(json.dumps(cfg, indent=2) + "\n")
PY
fi
if [[ -n "${VSOMEIP_LOG_LEVEL:-}" ]]; then
  sed -i "s/\"level\": \"warning\"/\"level\": \"${VSOMEIP_LOG_LEVEL}\"/" "${CFG_DIR}/bench.json"
fi

BAZEL=(bazel --batch)
BazelBuild() {
  (cd "${WS}" && "${BAZEL[@]}" build --config=opt --config=docker "$@")
}

# Built cc_binary path (runfiles wrapper); releases Bazel locks before exec.
BinForLabel() {
  local label="$1"
  local bin
  bin=$(cd "${WS}" && "${BAZEL[@]}" cquery "${label}" --config=opt --config=docker --output=files 2>/dev/null | tail -1)
  if [[ -z "${bin}" ]]; then
    echo "bazel cquery --output=files failed for ${label}" >&2
    exit 1
  fi
  if [[ "${bin}" != /* ]]; then
    bin="${WS}/${bin}"
  fi
  printf '%s' "${bin}"
}

RM_PID=""
cleanup() {
  if [[ -n "${RM_PID}" ]]; then
    kill "${RM_PID}" 2>/dev/null || true
    wait "${RM_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [[ "${STACK}" == "covesa" ]]; then
  echo "== bazel build routingmanagerd (baseline, local RM on pub and sub) ==" >&2
  BazelBuild "${RM_LABEL}"
  sed -e "s/@UNICAST@/${HOST_IP}/g" "${TPL}/routingmanagerd.json.in" > "${CFG_DIR}/routingmanagerd.json"
  export VSOMEIP_CONFIGURATION="${CFG_DIR}/routingmanagerd.json"
  export VSOMEIP_APPLICATION_NAME="routingmanagerd"
  rm_bin="$(BinForLabel "${RM_LABEL}")"
  echo "== exec routingmanagerd (unicast=${HOST_IP}) ==" >&2
  "${rm_bin}" &
  RM_PID=$!
  ready=0
  for _ in $(seq 1 300); do
    if ! kill -0 "${RM_PID}" 2>/dev/null; then
      echo "routingmanagerd (bazel run) exited before ready" >&2
      wait "${RM_PID}" || true
      exit 1
    fi
    if [[ -S /tmp/vsomeip-0 ]]; then
      ready=1
      break
    fi
    sleep 0.2
  done
  if [[ "${ready}" -ne 1 ]]; then
    echo "routingmanagerd socket /tmp/vsomeip-0 not ready in time" >&2
    exit 1
  fi
fi

bench_args=(
  --stack="${STACK}"
  --transport="${TRANSPORT}"
  --size="${SIZE}"
  --count="${COUNT}"
  --warmup="${WARMUP}"
  --rate-hz="${RATE_HZ}"
  --max-datagram="${MAX_DATAGRAM}"
)

export VSOMEIP_CONFIGURATION="${CFG_DIR}/bench.json"
if [[ "${ROLE}" == "service" ]]; then
  export VSOMEIP_APPLICATION_NAME="bench_service"
  rm -f /tmp/vsomeip-service-ready
else
  export VSOMEIP_APPLICATION_NAME="bench_client"
fi

echo "== bazel build ${BENCH} ==" >&2
BazelBuild "${BENCH}"
bench_bin="$(BinForLabel "${BENCH}")"
echo "== exec ${BENCH} ${bench_args[*]} ==" >&2
exec "${bench_bin}" "${bench_args[@]}"
