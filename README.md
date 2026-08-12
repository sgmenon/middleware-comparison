# Comparing automotive middleware

Let's talk about how GM chose a middleware for its next-generation vehicles.

The next generation is [SDV2](https://news.gm.com/home.detail.html/Pages/news/us/en/2025/oct/1022-SDV-GM-centralized-vehicle-computer-platform-electric-gas-vehicles.html) — fewer ECUs, and some seriously beefy compute nodes (think NVIDIA Thors) doing the heavy ADAS lifting. Some of the challenges that a middlware supporting an ADAS stack has to deal with are:

- It needs to wrangling huge amounts of data (from large sensor data, to model features maps getting shared).
- Theres's a lot of chatter going on, and there are many routes to deliver the messages. The middleware needs to always choose the smartest route.
- While most of that ADAS software now lives on big HPC-style boxes running Linux or QNX, the car still has a pile of smaller MCUs doing safety-critical work. Those worlds have to talk to each other and how the middleware deals with briding the worlds can be messy.

We'll walk through the use cases that actually showed up, how we turned those into requirements (and what we _didn't_ care about), and then look at three candidates — **DDS\***, **Zenoh**, and **Subspace** — side by side.

## The split personality of the vehicle

On the HPC side, life is pretty good: shared memory, big payloads, high-level OSes. We want something low-overhead that feels native there.

On the MCU side, a lot of boards run classic AUTOSAR, and even the ones that don't often still define their data exchange with ARXML. So the path of least resistance is **SOME/IP** — it's what those teams already speak.

SOME/IP is a middleware in its own right, and honestly not a great fit for most of what we want on the HPC side. So we weren't trying to rip SOME/IP out of the car. The real problem is: pick something good for the ADAS nodes, then bridge it to SOME/IP without lighting the CPU on fire when a lidar frame crosses the boundary.

## What we actually needed to do

Before writing a bunch of shall-statements, here are list of patterns that keep showing up:

1. **Everyday pub/sub between nodes** — periodic or async, nothing fancy.
2. **Safety-critical notifications** — here we usually want delivery guarantees, so RPCs are a natural fit.
3. **Mapping that pub/sub and those RPCs onto SOME/IP services** — especially for lerge sensor data, where an extra copy at the domain boundary really hurts.
4. **Diagnostics** — stuff that eventually gets marshalled toward a UDS server.
5. **Big payloads** — video, lidar. Drivers often write straight into shared memory; if the middleware copies that again, you feel it.
6. **Variable-sized messages** — without vaporizing history buffers or forcing awkward fixed ceilings.
7. **Mixed serialization** — SOME/IP, ROS, Protobuf, and our zero-copy SOME/IP library (`Zerosome`). The middleware shouldn't pick a favorite.
8. **Interporates with a good ecosystem of apps** - For data recording, snooping topics etc.

## How we turned that into requirements

We consciously made the choice of not picking a middlware and making a shoe-horn to fit our requirements around it. Its easy to ask "what does DDS offer?" and then come up with a feature lists. Instead we asked a few boring-but-useful questions, in roughly this order:

1. What _shapes_ of communication do we need? (pub/sub vs RPC, 1:1 vs 1:N)
2. What delivery guarantees do we actually care about — and what do we do when they fail?
3. Which wires exist between peers, and which one should win when more than one is available?
4. Who owns the bytes — the middleware's buffers, or driver memory we have to reuse?
5. What happens at boot when a thousand publishers show up at once? What happens if the broker dies?
6. What does tooling need to see without special-casing every app?
7. What happens when an the app crashes?

Everything else — giant QoS menus, in-middleware content filters, "must be peer-to-peer" purity — had to earn a seat. Most of it didn't.

### The must-haves

**Shape.** Low-overhead middleware for talking between ADAS nodes. The bread and butter is topic pub/sub, one-to-one or one-to-many. Many-to-many? Not really our problem.

**Delivery.** Most traffic can be best-effort. Some of it has to be reliable: the message gets there, or we hear about the failure. Safety-critical request/response should be RPCs, and those RPCs should always be reliable. Streaming RPCs matter too.

**Transports.** Always take the cheapest path available:

1. Zero-copy shared memory (same ECU)
2. PCIe
3. Ethernet — SOME/IP toward MCUs, otherwise plain TCP/UDP

The SOME/IP bridge itself is a whole blog post. For this comparison we just treat "crossing that boundary without a dumb copy" as a hard requirement and move on.

**Serialization.** Stay out of the user's serialization business. Zero-copy layouts are what a lot of ADAS folks want, but other teams live in Protobuf or ROS-land, and some use `Zerosome`. If someone's already on `Zerosome`, the SOME/IP bridge shouldn't force another copy just because we felt like it.

**Copies.** Be honest about copies. Ethernet has kernel RX/TX copies — fine, that's physics. Protobuf has a different in-memory vs wire shape — that's the user's choice. What we won't accept is the middleware copying an already-serialized blob for its own bookkeeping.

**Bounded memory.** Memory use needs a ceiling. Channels get a configurable history depth. Unreliable channels drop old stuff for new stuff. Reliable channels fail the send when history is full — they don't quietly grow forever.

**Variable size.** Variable-sized messages have to play nice with that history model. Resize the channel, and the messages still sitting in history shouldn't just disappear.

**Buffer ownership.** Normal case: the middleware owns the shared-memory history buffers. Awkward case: a camera driver that already wrote into its own memory. If we can't publish from that external buffer, we copy every frame, and that's a bad day. When buffers are external, it's fair that some "middleware manages the pool" features (like certain variable-size behaviors) aren't available.

**E2E checks.** Automotive likes checksums and counters. Users should be able to opt in. Failed checksum → discard by default. On reliable channels, we can still hand the bad message to the client and let them decide.

**Python.** We need bindings. Prototyping and testing without them is painful.

**Synchronization.** Locks are where middleware gets scary. Lock-free / coroutine-style designs are much easier to reason about — and an unreleased lock while you're holding shared memory can turn into a dead zone. We've been burned enough times to care.

**Discovery.** We shouldn't overload the CPU when hundreds or thousands of publishers come online at boot. With some peer-to-peer discovery protocols, the network traffic scales with the factorial of the number of peers, so even with small discovery messages the scale of a complex system can shake the system. Dyanmic discovery is a requirement but elegant scaling is also critical.

**Broker resilience.** If the middleware has a broker (unlike classic peer-to-peer DDS), and that broker crashes, existing channels should keep working. Sure, maybe you can't create new channels or resize buffers until it's back. After a restart, old channels should still be fine and new ones should be creatable again.

**Partitions.** Different partitions / domains — yes, we need that.

**Tooling.** We want to snoop channels, peek at payloads (when the serialization is something common), list what's out there, record, and play back.

**Liveliness.** Tell us when a publisher or subscriber shows up or disappears.

### Nice to haves

- Filtering on metadata (timestamps, instance IDs) for the rare many-to-many case
- Priorities — really only matters on the network, and DSCP/PCP usually does the job better than the middleware in our opinion.
- "Give me the newest message" as a first-class thing (DDS kinda gets there with reader-side history; Subspace just has it because queues are shared)

### Stuff we explicitly don't need

A few crowd favorites didn't make the cut:

- **Deadline QoS** — the client can do this. It doesn't need to live in the middleware.
- **Durability QoS** — we don't need late joiners to see history from before they existed.
- **Lifespan QoS** — fiddly, and easy to do per-message in the serialization layer.
- **Content filtering in the middleware** — that means the middleware has to understand your serialization, which fights the whole "stay serialization-agnostic" idea. With zero-copy, filtering on the client costs about the same anyway. On the sender side, just use different topics; we don't need that to be dynamic.
- **Must be peer-to-peer** — DDS's brokerless model is nice (one less thing to crash). But discovery gets ugly fast: everyone has to find everyone else, and that traffic scales badly even when the packets are small. We'll take a brokered design if it discovers calmly and survives a broker restart.

## The shortlist

| Candidate    | In one sentence                                                                                |
| ------------ | ---------------------------------------------------------------------------------------------- |
| **DDS\***    | Network-first pub/sub; shared memory via iceoryx (or vendor equivalents)                       |
| **Zenoh**    | Network-first pub/sub + query; optional routers; SHM as a transport opt                        |
| **Subspace** | Shared-memory-first pub/sub; TCP bridging (or custom PCIe bridging) when you leave the machine |

\* In this repo we wire up **Cyclone DDS + iceoryx** because it's open source and easy to build against. The comparison is meant to stand in for DDS more generally — you could swap in another implementation (Fast DDS, RTI Connext, DDS Micro, …) without changing the shape of the argument.

## Design center of gravity: shm-first vs network-first

This sounds fluffy until you live with it. Worth calling out before the scorecard:

- **DDS** and **Zenoh** are **network-first** designs. The core abstractions (RTPS / Zenoh sessions, discovery, congestion, routing) assume messages may cross a wire. Same-host shared memory is an _optimization bolted on_ — iceoryx under Cyclone, Zenoh’s own `zenoh-shm` path — that has to negotiate, fall back to copies, and stay coherent with the network story.
- **Subspace** is **shared-memory-first**. The happy path is POSIX shm channels on one machine; the server exists to set those up. Crossing machines is the bolt-on (TCP bridge / tunnels), not the other way around.

That matters for us because **most of our ADAS traffic is same-ECU**. Lidar frames, feature maps, camera buffers — they’re not going over Ethernet between two processes on the Thor; they’re (or should be) shared memory. A network-first stack can still do shm well, but the APIs, failure modes, buffer ownership, and “what is a channel” all grew up around packets. A shm-first stack grows up around slots, refcounts, and not touching your bytes.

When you read the truth table, keep that bias in mind: green cells for zero-copy on DDS/Zenoh often mean “yes, via an SHM sidecar.” On Subspace they mean “that _is_ the product.”

## Platform support

One thing worth getting straight before people start grading scorecards: **this middleware is for the HPC / ADAS boxes (Linux, QNX), not for the little MCUs.** MCUs already speak SOME/IP (or will, via the bridge). So "runs on a Cortex-M with 64K of RAM" is not a requirement for the candidates below — and Subspace not targeting MCUs is fine by us.

What we _do_ care about is POSIX-ish HPC targets, especially **Linux and QNX** on aarch64/x86_64.

| Platform                         | DDS\*                                                                    | Zenoh                                                                                                | Subspace                          |
| -------------------------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------- | --------------------------------- |
| Linux (HPC)                      | ✅                                                                       | ✅                                                                                                   | ✅                                |
| QNX (HPC)                        | ✅ (widely used in auto; vendor ports vary)                              | ⚠️ (zenoh-pico / zenoh-cpp via QNX ports / Conan recipes; full Rust stack less "batteries included") | ✅ (listed as a first-class port) |
| macOS / Android (dev)            | ⚠️ / varies                                                              | ✅-ish                                                                                               | ✅                                |
| Bare-metal / classic AUTOSAR MCU | ➖ (classic = SOME/IP; DDS Micro / Twin Oaks etc. are separate products) | ⚠️ (zenoh-pico exists; not our path)                                                                 | ➖ (not the job)                  |

So if someone says "Subspace only does POSIX, that's a gap" — for _our_ use case it isn't. The interesting platform question is really "how painful is QNX day-to-day," not "does it fit in an MCU."

## How they line up

|     | Meaning           |
| --- | ----------------- |
| ✅  | Looks good        |
| ⚠️  | Partial / caveats |
| ❌  | Weak or missing   |
| ➖  | Not applicable    |
| ❓  | Still squinting   |

This is a first pass from docs and APIs, not from our own benches.

| Requirement                              | DDS\*                                               | Zenoh                                      | Subspace                                                                                        |
| ---------------------------------------- | :-------------------------------------------------- | :----------------------------------------- | :---------------------------------------------------------------------------------------------- |
| Topic pub/sub (1:1, 1:N)                 | ✅                                                  | ✅                                         | ✅                                                                                              |
| Best-effort + reliable delivery          | ✅                                                  | ✅                                         | ✅                                                                                              |
| Reliable RPC                             | ⚠️ need to wrap it                                  | ✅                                         | ✅                                                                                              |
| Streaming RPC                            | ❌                                                  | ❌                                         | ✅                                                                                              |
| Zero-copy shared memory (same ECU)       | ✅                                                  | ✅                                         | ✅                                                                                              |
| PCIe as a first-class path               | ❌                                                  |                                            | ⚠️ (with a custom "tunnel")                                                                     |
| Ethernet (TCP/UDP)                       | ✅                                                  | ✅                                         | ✅ (with a tunnel)                                                                              |
| Efficient SOME/IP domain crossing        | ⚠️                                                  | ⚠️                                         | ✅ (beacuse its serailization agnostic and having external buffer management)                   |
| Serialization-agnostic payloads          | ❌                                                  | ✅                                         | ✅                                                                                              |
| Middleware doesn't copy serialized bytes | ⚠️                                                  | ⚠️                                         | ✅                                                                                              |
| Bounded history / memory ceiling         | ✅                                                  | ✅                                         | ✅                                                                                              |
| Variable-size without wiping history     | ⚠️ (not in the ports we tried)                      | ✅                                         | ✅                                                                                              |
| Reuse externally managed buffers         | ⚠️ (with proprietary stacks)                        | ❌                                         | ✅                                                                                              |
| Optional checksums / E2E hooks           | ⚠️ (with proprietary extensions)                    | ❌                                         | ✅                                                                                              |
| Python bindings                          | ✅                                                  | ✅                                         | ✅                                                                                              |
| Lock-free / coroutine-friendly core      | ⚠️ (maybe with a propreitary version)               | ⚠️ (Rust stack - side-steps some problems) | ✅                                                                                              |
| Discovery calm at large boot fan-out     | ⚠️ (have to use static routing for this)            | ✅ (client→router) · ⚠️ (peer mesh)        | ✅                                                                                              |
| Existing channels survive broker death   | ➖ (P2P)                                            | ➖ (PTP mode) · ❌ (router mode)           | ✅                                                                                              |
| Partitions / domains                     | ✅                                                  | ✅                                         | ✅ (just use a different socket)                                                                |
| Tooling (snoop / list / record / play)   | ✅                                                  | ⚠️                                         | ✅ (tooling not open source yet)                                                                |
| Liveliness (join/leave notifications)    | ✅                                                  | ✅                                         | ⚠️ (there's a separate channel with network changes we subscrive to and filter for our channel) |
| Implementation / codebase size           | ⚠️ (reasonable for ports like DDS Micro or FastDDS) | ✅                                         | ✅                                                                                              |
| Protocol complexity                      | ⚠️                                                  | ⚠️                                         | ✅                                                                                              |
| Linux + QNX on HPC ECUs                  | ✅                                                  | ⚠️ (QNX via ports)                         | ✅                                                                                              |

\* Same footnote as above: this column is "DDS the family." Lab bench here is Cyclone + iceoryx; another DDS stack can be dropped in.

### Digging into the interesting cells

- **RPC / streaming RPC.** Zenoh's queryables are a clean request/reply story. Subspace has actual RPC libraries on top of channels. DDS can do request/reply, but it feels bolted on, and true streaming RPC is a bit of a DIY project on all three.
- **Serialization and copies.** Subspace is proudly "bring your own bytes." Zenoh is in a similar opaque-payload camp. Classic DDS really wants IDL/CDR; iceoryx loans help on the shared-memory path, but "here's a blob, don't touch it" is less natural unless you force topics to be opaque.
- **External buffers.** Subspace's split buffers are aimed at external allocators and driver pools. Cyclone's loan API loans _out of_ iceoryx's pool — it doesn't wrap arbitrary external memory. Zenoh SHM is mostly provider-managed too
- **Checksums.** Subspace has opt-in CRC (and custom callbacks). DDS and Zenoh mostly say "do E2E in the app", or with DDS, pripreitary stacks do this via extensions.
- **Discovery vs liveliness.** Same topology tradeoff as broker death, just flipped: Zenoh's calm boot story is really **client→router** (clients attach to one place; they don't all scout each other). A big **peer mesh** is back in DDS-land — scouting traffic scales with who needs to find whom, and a thousand peers showing up at once is not free. DDS has real liveliness QoS, but peer discovery can get spicy at scale (static routing / discovery servers help — hence the yellow). Subspace keeps discovery simple via the server… but it doesn't really notify apps when publishers come and go the DDS/Zenoh way (there's a separate network-changes channel you filter). Zenoh liveliness tokens are first-class either way.
- **Broker death.** Zenoh isn't "P2P or nothing" — every node picks a role: **peer**, **client**, or **router**. Peer↔peer (optionally with routers in the mix) can look a lot like DDS P2P: no broker in the path, so there's nothing to kill for those links. Client→router is the brokered shape; if that router dies, those clients lose the attachment until reconnect. Subspace is different again: it _has_ a local server, but the shadow process is built so a restart can pick shared memory back up without killing existing channels. Pure DDS sidesteps the broker entirely.
- **SOME/IP.** None of these _are_ SOME/IP. Everyone needs a bridge. The open question is how cheap that bridge can be for big sensor data.
- **Complexity (two different axes).** DDS loses on both bulk and brain-damage: big implementations _and_ a fat QoS/discovery protocol surface (Micro trims the binary, not the mental model as much as you'd hope). Zenoh's **codebase** is leaner and nicer to approach than a full DDS stack — that's why implementation size goes green — but the **protocol** still has real surface area (roles, scouting, keyexprs, congestion control, SHM negotiation, advanced pub/sub). Subspace wins both: small server/client and a simple channel model.
- **Shm-first vs network-first.** Most of our load is same-ECU. Subspace’s design center is shm channels; DDS/Zenoh add shm on top of a network protocol. Same checkbox can hide a different architecture.
- **Platforms.** We're picking middleware for Linux/QNX HPC nodes, not for MCUs. Subspace's POSIX/shm world (Linux, QNX, macOS, Android) matches that job. Classic AUTOSAR micros speak SOME/IP, not DDS — DDS in Adaptive AUTOSAR is still POSIX-class HW, and "DDS on an MCU" usually means a separate product (DDS Micro, Twin Oaks, …), not Cyclone-on-Thor. Zenoh can reach down via zenoh-pico and has QNX community ports, but "works on our QNX Thor image tomorrow" still wants a hands-on check.

## What's next

1. Turn the soft cells into measured claims — latency, copies, discovery CPU at boot, "what happens if we kill the broker."
2. Sketch the SOME/IP bridge and figure out where zero-copy can realistically survive the domain crossing.
3. Add some architecture diagrams for each candidate's data path (shm vs network vs bridge).

This repo is the lab bench for that work. More soon.
