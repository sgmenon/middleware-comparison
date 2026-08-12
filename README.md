# Comparing automotive middleware

## Use Cases

GM's [next vehicle generation (SDV2)](https://news.gm.com/home.detail.html/Pages/news/us/en/2025/oct/1022-SDV-GM-centralized-vehicle-computer-platform-electric-gas-vehicles.html) features fewer ECUs and some pretty powerful compute nodes, like NVIDIA Thors, for doing the heavy lifting of ADAS.

What this means is that most software components that do ADAS work are primarily running on these large HPC-style ECUs, that run high level operating systems like Linux or QNX.

The new vehcile architectre still has a lot of smaller microcontrollers (MCUs), and many of these run safety critical workloads. These MCUs need to talk to the rest of the ADAS stack.

1. Routine pub/sub between Nodes. This can be periodic or asynchronous comms.
2. Safety critical notifications. These often require message recept guarantees, so RPCs are a good use case for these.
3. Communication of daignositic information (which is later marshalled to a UDS server)
4. Transmission of large data (video and lidar frames). Handling of this data can be a large load on the CPU if we do any copies. Often drivers directly write into shared memory
5. Dealing with variable sized elements efficiently
6. Some Nodes use SOME/IP serialization, some use ROS serialization, some use Protobuf. Lets avoid the copy and keep the middleware serialization agnostic

## Requirements

Need a low overhead middleware for sharing data between its ADAS software components (Nodes)

The prototypipcal pattern of comms is Topic based Pub/Sub - one-to-one or one-to-many. Many-to-Many is not a particularly important use case for us.

Most communications should be best-effort, though some communication will be expected to be reliable. Reliable messaging means that delivery of the message shall be guaranteed, and for whatever reason if communications fail we need to know about the failure so we can take necessary action.

There are certain safety critical communications that need to be in the form of RPCs. All RPC communication should be reliable.

Streaming RPCs are also important.

Comminucation should follow the channel of least latency whenever possible. In theis order of preference:

- 0 copy shared memory (applicable when nodes are on the same ECU)
- PCIe
- Ethernet
  - SOME/IP (for comms with MCUs). How we bridge the middleware and map SOME/IP concepts to a more generic middleware is very intersting and is a subject of another deep dive.
  - TCP/IP or UDP

All communcation shall be serialization agnostic. Zero copy serializations are preferred in the GM ADAS team but some teams like to use Protobufs while others may like ROS serialization.

Copies in the comms channel is kept to the minimum. Yes at the kernel level, if use use Ethernet, we have to copy from RX and TX buffers, and yes some users like to use data structures like Protocols Buffers which do have a different in-memory and serialized represetntation, but the actual middleware shall not copy a serialized type anywhere.

Memory usage shall not scale beyond a ceiling. Specifically, communication channels need to have a configurable history length. Unreliable messages that are older shall be dropped in favor of newer ones. Reliable messages will fail to send if the history buffer is full.

Support for Variable Sized Messages in confluence with the history requirement i mentioned above. When resizes happen, previous messages in the history buffer should not vanish.

Particularly when dealing with shared memory, most use cases would call for the middelware owning, and managing, the buffers (history buffers say) used for data tranfer. However, some use cases, for instance, camera drivers that directly write into a driver managed region of memory, call for the middlware to reuse an externally allocated buffer. If we don't do support reusing an externally managed buffer, then we'll incur a copy which, for sensors like cameras, can be a substantial CPU load. Of course, if memory is externally managed, then some features of the middleware that call for internal buffer management (eg. managing variable sized messages) are reasonable to to not be available.

In order to conform to Automotive E2E checks, messages may need to have checksums and counters, so the users shall have the ability to opt-into using checksums. If a checksum fails the message shall be discarded. Reliable messages with failing E2E checks still get delivered, the client can discard it.

Python bindings are super important for prototying and testing, so the middleware shall to have these too.

In our experience, synchronization is often the trickest part of writing a solid, bug free middeware stack, and the problem is greatly simplified if we just avoid needing the do synchronization by using lock-free techniqueues like co-routines. Without this the stack is harder to guarantee. Also an unreleased lock, when holding onto shared memory, can result in a dead-zone in memory.

Discovery should not cause an dangerous CPU spike when 100s or 1000s of publishers come online at boot. The simpler discovery is the better.

For middlware that have a broker (not peer-to-peer like DDS) when the broker's server crashes, for whatever reason, it is important that existing channels are not disrupted. Some features may not work when the server is down (discovery, new channel creation, crash minitoring, buffer resizing etc.). If the server is restarted, new channels should be creatible but old ones should still be functional.

Partitions or different domains should be supported

The MW needs to work hand in hand with tooling for things like:

- Snooping channels
- Snooping the payload (if its in a common serialization format)
- Listing channels
- Data recording
- Data playback

- Liveliness QOS: ie. notifications when someone subscribes to a topic or drops off.

## Nice to haves

Filtering on metadata: eg. timestamps, instance ids (for many-to-many use cases).

Priorities - but this is ONLY relevant for networking (not shared memory) and can be effectiveily done use DSCP or PCP priorities

Ability to get the newest message when available. DDS allows you to set the history at the reader side, subspace has shared queues (since its 0 copy) so it has this as a separate feature.

### Non-requirements

Many QOS policiies

- Deadline QOS: this kind of logic is easily handled on the part of the client and doesn't really belong
- Durability QOS: After something is sent, we don't need anyone to hold on to old data. A presumed use case is that a new publisher comes online and should see data from "back in time before it came online". This is not an imporatnt use case for our systmes.
- Lifespan qos - pretty complicated and can be easily implement per-message within the serialization. This doesn't belong in the mw.

Filtering packets at the middlware level. Filtering requires the middleware to be aware of the serialization, so violates some of our requirements. Additionally, its relatively trivial for a applications to do filtering by themselves, and given the requirement that our middlware prioritizes 0 copy data transfers, the cost to doing it on the client side is equivalent to doing it in the middlware. Filtering at the sender side is better implemented by just making different topics, this is not the kind of thing that needs to be dynamic in our use case.

Needing a peer-to-peer middlware: DDS is peer to peer, so it doesn't need a broker application. While this is desirable - the lack of a broker means that a failure mode is eliminiated. However, this complicates service discovery - everyone in the system must discover everyone else, so the traffic received per Node, during discovery, scales as a factorial of the number of nodes, which can be disasterous for a large system, even if discovery packets are relatively small.

### Candidates

- DDS (Cyclone with iceoryx)
- Zenoh
- Subspace
