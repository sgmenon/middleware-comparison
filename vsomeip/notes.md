# vsomeip benchmark (separate from Subspace / Zenoh / Cyclone)

SOME/IP through vsomeip is a different problem domain (AUTOSAR-style services, routing
manager, service discovery, E2E). We keep experiments under `vsomeip/` instead of mixing
them into the main middleware comparison.

## Fork vs baseline (Bazel)

`MODULE.bazel` loads `third_party/vsomeip/extension.bzl`, which registers:

| Repo               | Source                                                                     | Role                                                    |
| ------------------ | -------------------------------------------------------------------------- | ------------------------------------------------------- |
| `@vsomeip_sgmenon` | [sgmenon/vsomeip](https://github.com/sgmenon/vsomeip) @ `598f3a79…` (main) | **Improved** GM-style stack (no external RM)            |
| `@vsomeip_covesa`  | Same fork @ `e235a303…`                                                    | **Baseline** COVESA-style snapshot w/ Bazel BUILD files |

The covesa pin is fetched from the fork because GitHub returns 404 for that commit on
`COVESA/vsomeip` archives; the SHA is an ancestor of main in the fork.

To move the sgmenon pin to latest main:

```bash
COMMIT=$(curl -sL https://api.github.com/repos/sgmenon/vsomeip/commits/main | jq -r .sha)
curl -sL "https://github.com/sgmenon/vsomeip/archive/${COMMIT}.tar.gz" -o /tmp/vsomeip.tgz
python3 - <<EOF
import hashlib, base64, pathlib
d = hashlib.sha256(pathlib.Path("/tmp/vsomeip.tgz").read_bytes()).digest()
print("integrity = sha256-" + base64.b64encode(d).decode())
EOF
# Update _VSOMEIP_SGMENON_COMMIT + integrity in third_party/vsomeip/extension.bzl
```

## Improvements (from `vsomeip-improvements-presentation`)

[Slide deck](https://sgmenon.github.io/vsomeip-improvements-presentation/) from the October 2026 `COVESA AMA` talk.

## Workload

- One service / instance / event group / event (see `bench/constants.h`).
- **UDP only** (`RT_UNRELIABLE`): service config exposes a single **unreliable** port (no TCP
  `reliable` block). Payloads larger than `--max-datagram` are split in **user space**
  (`bench/fragment.h`) and reassembled on the subscriber. **No SOME/IP-TP.**

**What we measure:** latency for one **logical frame** (e.g. lidar scan blob)—timestamp on the full
`size`-byte payload before fragmentation, latency recorded only after the subscriber reassembles the
same `size`. The table/chart summarize a **distribution** (mean, p50, p99 over many frames).

**Size grid** matches `notes/benchmarks.md` ReliablePingPong: 64 B, 1 KiB, 16 KiB, 64 KiB, 256 KiB,
1 MiB, 4 MiB (`docker/size_ladder.sh`). **`rate_hz` is a fixed pace** so frames are not back-to-back;
it is not swept here (that would be a separate CPU/load study).

### Process layout under test (Docker)

| `STACK`   | Library      | Pub container                                               | Sub container                                             |
| --------- | ------------ | ----------------------------------------------------------- | --------------------------------------------------------- |
| `covesa`  | Baseline pin | `routingmanagerd` + `event_service_covesa`                  | `routingmanagerd` + `event_client_covesa`                 |
| `sgmenon` | Fork main    | `event_service_sgmenon` only (`"routing": "bench_service"`) | `event_client_sgmenon` only (`"routing": "bench_client"`) |

Config: `vsomeip/config/vsomeip.json.in` (pub) or `vsomeip_client.json.in` (sub) → `bench.json` (`@UNICAST@`, `@ROUTING@`).
Pub keeps the UDP `services` block; sub drops `services` and adds a `clients` port range. **covesa**
`routingmanagerd` uses the same `bench.json` as the bench binary (`VSOMEIP_APPLICATION_NAME` selects
the process).

CSV columns (subscriber prints one line):

`stack,size,rate_hz,n,mean_us,p50_us,p99_us,gap_count` (UDP event notify only)

## Docker (Bazel inside the container)

Two containers on a fixed `/24` (pub @ `172.29.0.3`, sub @ `172.29.0.2`). Containers start as
root with **`cap_add: NET_ADMIN`** so `multicast_setup.sh` can configure the veth; entrypoint then
**`setpriv`** to **`HOST_UID`/`HOST_GID`** (from `run.sh`) for Bazel and the bench binaries. Compose
`user:` plus `cap_add` alone does not grant effective caps to an unprivileged process.
Pub keeps the `services` block; sub drops it and adds `clients` for UDP subscriber ports.

### SD, multicast routes, and Docker

Two separate issues often get lumped together:

1. **Classic vsomeip / netlink** — On Linux, routing managers watch RTNETLINK for a **unicast
   route whose destination covers the configured SD multicast** (`check_sd_multicast_route_match`
   in `netlink_connector`). Until that route exists, SD may not treat the interface as ready.
   Upstream docs and docker tests therefore add something like
   `ip route add 224.0.0.0/4 dev eth0` before starting apps. The **covesa baseline pin** behaves
   like this generation of stack.

2. **Docker bridge** — Even with a 224/4 route, containers often need **`rp_filter=0`** on the
   veth or OFFERs from the peer never survive reverse-path filtering. Upstream
   `test/network_tests/docker_tests/docker_infra/entrypoint.sh` does both route + sysctl.

**sgmenon main** has been moving away from insisting on that explicit multicast route (newer
commits relax netlink gating / join behavior). After bumping `_VSOMEIP_SGMENON_COMMIT`, you can
set `VSOMEIP_DOCKER_NET_SETUP=0` to skip `multicast_setup.sh` and verify SD without the 224/4
route; keep setup enabled for **covesa** and for apples-to-apples Docker runs until both pins
behave the same on your kernel.

```bash
# Baseline: dual routingmanagerd (COVESA-style)
STACK=covesa SIZE=4096 RATE_HZ=1000 COUNT=1000 vsomeip/docker/run.sh

# Improved: no routingmanagerd (GM-style per-app routing on the fork)
STACK=sgmenon SIZE=65536 RATE_HZ=500 COUNT=2000 vsomeip/docker/run.sh

# Size ladder → bench_results/vsomeip/ (set STACK=covesa|sgmenon)
STACK=sgmenon vsomeip/docker/sweep.sh

# Snapshot markdown (both stacks, size ladder)
vsomeip/docker/run_snapshot.sh
```
