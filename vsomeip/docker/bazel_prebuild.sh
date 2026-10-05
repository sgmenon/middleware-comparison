#!/usr/bin/env bash
# Warm shared ~/.cache/bazel-output before compose (same flags as container entrypoint).
set -euo pipefail

vsomeip_bazel_prebuild() {
  local root="${1:?repo root}"

  export BAZEL_OUTPUT_USER_ROOT="${BAZEL_OUTPUT_USER_ROOT:-${HOME}/.cache/bazel-output}"

  local extra_configs=()
  if [[ -n "${VSOMEIP_TRACY:-}" ]]; then
    extra_configs+=(--config="${BAZEL_CONFIG_TRACY:-tracy_docker}")
  fi
  echo "== bazel build //vsomeip/... (host prewarm) ==" >&2
  (cd "${root}" && bazel build --config=opt --config=docker "${extra_configs[@]}" \
    //vsomeip/... \
    @vsomeip_covesa//examples/routingmanagerd:routingmanagerd)
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  vsomeip_bazel_prebuild "${ROOT}"
fi
