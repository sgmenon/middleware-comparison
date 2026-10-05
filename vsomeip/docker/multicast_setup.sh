#!/usr/bin/env bash
# Docker harness network prep for SOME/IP SD (not part of the vsomeip binary):
#   - 224.0.0.0/4 -> eth0 satisfies classic netlink "SD multicast route" checks in older vsomeip.
#   - rp_filter=0 lets multicast from the other container on the bridge reach this one.
# Skip entirely with VSOMEIP_DOCKER_NET_SETUP=0 (e.g. testing sgmenon without explicit 224/4).
set -euo pipefail

if [[ "${VSOMEIP_DOCKER_NET_SETUP:-1}" == "0" ]]; then
  echo "vsomeip multicast: skipped (VSOMEIP_DOCKER_NET_SETUP=0)" >&2
  exit 0
fi

IFACE="$(ip route | awk '/default/ {print $5; exit}')"
if [[ -z "${IFACE}" ]]; then
  IFACE="$(ip -o link show | awk -F': ' '{print $2}' | grep -v '^lo$' | head -1)"
fi
if [[ -z "${IFACE}" ]]; then
  echo "multicast_setup: no network interface" >&2
  exit 1
fi

if ! ip route show | grep -q '224.0.0.0/4'; then
  ip route add 224.0.0.0/4 dev "${IFACE}"
fi

sysctl -w net.ipv4.conf.all.rp_filter=0 2>/dev/null || true
sysctl -w net.ipv4.conf.default.rp_filter=0 2>/dev/null || true
sysctl -w "net.ipv4.conf.${IFACE}.rp_filter=0" 2>/dev/null || true

echo "vsomeip multicast: iface=${IFACE} ip=$(hostname -I | awk '{print $1}')" >&2
