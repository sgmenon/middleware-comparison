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

**Size grid** matches `notes/benchmarks.md` ReliablePingPong through 4 MiB, plus **10 MiB**
(`docker/size_ladder.sh`). **`rate_hz` is a fixed pace** so frames are not back-to-back;
it is not swept here (that would be a separate CPU/load study).

### Process layout under test (Docker)

| `STACK`   | Library      | Pub container                                               | Sub container                                             |
| --------- | ------------ | ----------------------------------------------------------- | --------------------------------------------------------- |
| `covesa`  | Baseline pin | `routingmanagerd` + `event_service_covesa`                  | `routingmanagerd` + `event_client_covesa`                 |
| `sgmenon` | Fork main    | `event_service_sgmenon` only (`"routing": "bench_service"`) | `event_client_sgmenon` only (`"routing": "bench_client"`) |

Config: `vsomeip/config/vsomeip.json.in` (pub) or `vsomeip_client.json.in` (sub) → `bench.json` (`@UNICAST@`, `@ROUTING@`).
**E2E:** Profile **P04** on event `0x8001` / service `0xbeef` — pub `variant: protector`, sub `variant: checker`, `crc_offset` 64 bits (header after byte 0), `max_data_length` 83886080 bits (10 MiB ladder). Application payloads stay hole-free; vsomeip inserts the E2E header on send. Bazel links **`libvsomeip3-e2e.so.3`** on bench binaries and covesa `routingmanagerd` (same pattern as SD/cfg plugins); Docker sets `LD_LIBRARY_PATH` to the built plugin directory.
Pub keeps the UDP `services` block; sub drops `services` and adds a `clients` port range. **covesa**
`routingmanagerd` uses the same `bench.json` as the bench binary (`VSOMEIP_APPLICATION_NAME` selects
the process).

**History CSV** (append-only, default `bench_results/vsomeip/snapshot.csv`):

`recorded_utc,git_sha,run_label,count,warmup,stack,size,rate_hz,n,mean_us,p50_us,p99_us,gap_count`

Each `run_snapshot.sh` invocation sets `RUN_LABEL` (or pass `RUN_LABEL=myexperiment`). Override path with `SNAPSHOT_CSV=…`. Bench binary still prints the trailing eight fields; the shell adds metadata.

**vs Docker net (`notes/benchmarks.md`):** that table is **not** apples-to-apples. Net CLIs are **ping-pong** (pub writes, waits for echo, reports **RTT/2**). This harness is **one-way** event notify (stamp on pub before `notify`, receive time in the sub’s notification handler after reassembly). Discovery/SD is setup-only—it is **not** in the measured send→receive interval once subscribed. The gap at 64 B (~**5 ms** one-way here vs ~**30–75 µs** RTT/2 there) still points at **per-message SOME/IP + vsomeip runtime work** (serialization, dispatch, allocations, routing—and on covesa **app↔routingmanagerd** on every notify), not at “SD tax.” Payload copies matter more as frames grow; they do not explain the small-payload floor. A fairer cross-middleware comparison would be **one-way** pub→sub with the same stamp semantics on each stack.

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

````bash
# Baseline: dual routingmanagerd (COVESA-style)
STACK=covesa SIZE=4096 RATE_HZ=1000 COUNT=1000 vsomeip/docker/run.sh

# Improved: no routingmanagerd (GM-style per-app routing on the fork)
STACK=sgmenon SIZE=65536 RATE_HZ=500 COUNT=2000 vsomeip/docker/run.sh

# Size ladder → bench_results/vsomeip/ (set STACK=covesa|sgmenon)
STACK=sgmenon vsomeip/docker/sweep.sh

# Snapshot markdown (both stacks, size ladder)
vsomeip/docker/run_snapshot.sh

**Do not** run snapshot, `run_profile.sh` (Tracy), and `run.sh` in parallel — they share `mw_vsomeip_pub` / `mw_vsomeip_sub`. `run.sh` takes an exclusive lock (`/tmp/mw_vsomeip_compose.lock`); a second invocation exits until the first finishes or you `docker compose --profile vsomeip down`.

# Incremental: append 10 MiB only, re-render md from latest CSV rows per size
SIZES=10485760 RUN_LABEL=10mib-test vsomeip/docker/run_snapshot.sh

### Tracy profiling

Wiring matches `DEVOPS.ccu-stack/third_party/tracy` (`--config=tracy`, depend on `@tracy`). Bench binaries include `ZoneScopedN` on publish / notify / reassemble / handler; `TracyPlot` records sampled frame latency on the subscriber.

```bash
# Host terminal 1: tracy-capture (or Tracy GUI), port 8086
tracy-capture -o /tmp/vsomeip.tracy -a 0.0.0.0

# Host terminal 2 — TRACY_CLIENT_ADDRESS = IP containers use to reach the host
TRACY_CLIENT_ADDRESS=172.17.0.1 VSOMEIP_TRACY=1 STACK=sgmenon SIZE=64 COUNT=30 \
  vsomeip/docker/run_profile.sh
````

Zones in this repo show **harness vs vsomeip** split (e.g. time inside `notify()` vs gaps before `on_message`). To see routing/UDP inside the stack, rebuild `@vsomeip_sgmenon` / `@vsomeip_covesa` with `--config=tracy` and add `ZoneScopedN` in the library (same pattern as ccu-stack camera code).

Details: `third_party/tracy/README.md`.

```

```
