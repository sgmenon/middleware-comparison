#!/usr/bin/env bash
# Bazel in-container: shared ~/.cache/bazel-output. Use build + exec (not `bazel run`) for long-lived
# bench/R routingmanagerd processes — `bazel run` keeps the output-base lock until the child exits.
set -euo pipefail

# cap_add NET_ADMIN is effective for root only; drop to caller UID before Bazel (see run.sh).
if [[ "$(id -u)" -eq 0 && -n "${HOST_UID:-}" && -n "${HOST_GID:-}" && "${VSOMEIP_DROPPED:-}" != 1 ]]; then
  if [[ "${VSOMEIP_DOCKER_NET_SETUP:-1}" != "0" ]]; then
    /usr/local/bin/multicast_setup.sh
  fi
  export VSOMEIP_DROPPED=1
  exec setpriv --reuid="${HOST_UID}" --regid="${HOST_GID}" --clear-groups \
    env USER="${BENCH_USER:-bench}" LOGNAME="${BENCH_USER:-bench}" HOME="${HOME:-/home/bench}" \
    /usr/local/bin/bazel_entrypoint.sh
fi

ROLE="${ROLE:?ROLE must be service (pub) or client (sub)}"
STACK="${STACK:?STACK must be covesa (baseline) or sgmenon (improved)}"
SIZE="${SIZE:-4096}"
COUNT="${COUNT:-1000}"
RATE_HZ="${RATE_HZ:-1000}"
WARMUP="${WARMUP:-50}"
MAX_DATAGRAM="${MAX_DATAGRAM:-1400}"

CLIENT_IP="${CLIENT_IP:-172.29.0.2}"
SERVICE_IP="${SERVICE_IP:-172.29.0.3}"

WS="${WORKSPACE:-/workspace}"
export HOME="${HOME:-/home/bench}"
export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-${HOME}/.cache/bazel-output}"
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

CFG_TPL="${TPL}/vsomeip.json.in"
if [[ "${ROLE}" == "client" ]]; then
  CFG_TPL="${TPL}/vsomeip_client.json.in"
fi
sed -e "s/@UNICAST@/${UNICAST}/g" -e "s/@ROUTING@/${ROUTING}/g" \
  "${CFG_TPL}" > "${CFG_DIR}/bench.json"
if [[ -n "${VSOMEIP_LOG_LEVEL:-}" ]]; then
  sed -i "s/\"level\": \"warning\"/\"level\": \"${VSOMEIP_LOG_LEVEL}\"/" "${CFG_DIR}/bench.json"
fi

BAZEL=(bazel --batch)
BAZEL_BENCH_CONFIGS=(--config=opt --config=docker)
if [[ -n "${VSOMEIP_TRACY:-}" ]]; then
  BAZEL_BENCH_CONFIGS+=(--config="${BAZEL_CONFIG_TRACY:-tracy_docker}")
  export TRACY_CLIENT_ADDRESS="${TRACY_CLIENT_ADDRESS:-172.17.0.1}"
  BAZEL_BENCH_CONFIGS+=(--copt=-DTRACY_CLIENT_ADDRESS=\"${TRACY_CLIENT_ADDRESS}\")
fi
BazelBuild() {
  (cd "${WS}" && "${BAZEL[@]}" build "${BAZEL_BENCH_CONFIGS[@]}" "$@")
}

# Built cc_binary path (runfiles wrapper); releases Bazel locks before exec.
BinForLabel() {
  local label="$1"
  local bin
  bin=$(cd "${WS}" && "${BAZEL[@]}" cquery "${label}" "${BAZEL_BENCH_CONFIGS[@]}" --output=files 2>/dev/null | tail -1)
  if [[ -z "${bin}" ]]; then
    echo "bazel cquery --output=files failed for ${label}" >&2
    exit 1
  fi
  if [[ "${bin}" != /* ]]; then
    bin="${WS}/${bin}"
  fi
  printf '%s' "${bin}"
}

VsomeipPluginLibDir() {
  local repo="$1"
  local so
  so=$(cd "${WS}" && "${BAZEL[@]}" cquery "@vsomeip_${repo}//:vsomeip3-sd" "${BAZEL_BENCH_CONFIGS[@]}" --output=files 2>/dev/null | tail -1)
  if [[ -z "${so}" ]]; then
    echo "bazel cquery failed for @vsomeip_${repo}//:vsomeip3-sd" >&2
    exit 1
  fi
  if [[ "${so}" != /* ]]; then
    so="${WS}/${so}"
  fi
  dirname "${so}"
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
  export VSOMEIP_CONFIGURATION="${CFG_DIR}/bench.json"
  export VSOMEIP_APPLICATION_NAME="routingmanagerd"
  rm_bin="$(BinForLabel "${RM_LABEL}")"
  export LD_LIBRARY_PATH="$(VsomeipPluginLibDir covesa)${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
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
  --size="${SIZE}"
  --count="${COUNT}"
  --warmup="${WARMUP}"
  --rate-hz="${RATE_HZ}"
  --max-datagram="${MAX_DATAGRAM}"
)

export VSOMEIP_CONFIGURATION="${CFG_DIR}/bench.json"
export VSOMEIP_BENCH_SYNC_DIR="${WS}/vsomeip/docker"
BENCH_DONE="${WS}/vsomeip/docker/.bench-sub-done"
BENCH_SUB_READY="${VSOMEIP_BENCH_SYNC_DIR}/.bench-sub-ready"
if [[ "${ROLE}" == "service" ]]; then
  export VSOMEIP_APPLICATION_NAME="bench_service"
  rm -f /tmp/vsomeip-service-ready "${BENCH_DONE}" "${BENCH_SUB_READY}"
else
  export VSOMEIP_APPLICATION_NAME="bench_client"
fi

echo "== bazel build ${BENCH} ==" >&2
BazelBuild "${BENCH}"
bench_bin="$(BinForLabel "${BENCH}")"
export LD_LIBRARY_PATH="$(VsomeipPluginLibDir "${STACK}")${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
echo "== exec ${BENCH} ${bench_args[*]} ==" >&2
if [[ "${ROLE}" == "service" ]]; then
  # Subscriber may still be starting (Bazel build) or collecting samples; do not exit and tear down compose.
  "${bench_bin}" "${bench_args[@]}"
  echo "service bench finished; waiting for subscriber (marker ${BENCH_DONE})" >&2
  pub_wait_sec=240
  if [[ "${SIZE}" -ge 10485760 ]]; then
    pub_wait_sec=3600
  elif [[ "${SIZE}" -ge 4194304 ]]; then
    pub_wait_sec=900
  fi
  for _ in $(seq 1 "${pub_wait_sec}"); do
    [[ -f "${BENCH_DONE}" ]] && break
    sleep 1
  done
else
  exec "${bench_bin}" "${bench_args[@]}"
fi
