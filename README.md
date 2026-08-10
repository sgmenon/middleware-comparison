# Comparing automotive middleware

## Use Cases

## Requirements

GM needs a low overhead middleware for sharing data between its ADAS software components (Nodes)

The prototypipcal pattern of comms is Topic based Pub/Sub - one-to-one or one-to-many. Many-to-Many is not a particularly important use case for us.

Most communications should be best-effort, though some communication will be expected to be reliable. Reliable messaging means that delivery of the message shall be guaranteed, and for whatever reason if communications fail we need to know about the failure so we can take necessary action.

There are certain safety critical communications that need to be in the form of RPCs. All RPC communication should be reliable.

Comminucation should follow the channel of least latency whenever possible. In theis order of preference:

- 0 copy shared memory (applicable when nodes are on the same ECU)
- PCIe
- Ethernet (UDP or TCP/IP)

All communcation shall be serialization agnostic. Zero copy serializations are preferred in the GM ADAS team but some teams like to use Protobufs while others may like ROS serialization.

Copies in the comms channel is kept to the minimum. Yes at the kernel level, if use use Ethernet, we have to copy from RX and TX buffers, and yes some users like to use data structures like Protocols Buffers which do have a different in-memory and serialized represetntation, but the actual middleware shall not copy a serialized type anywhere.

Memory usage shall not scale beyond a ceiling. Related to this, communication channels need to have a configurable history length. Unreliable messages that are older shall be dropped in favor of newer ones. Reliable messages will fail to send if the history buffer is full.

In order to conform to Automotive E2E checks, messages may need to have checksums and counters, so the users shall have the ability to opt-into using checksums. If a checksum fails the message shall be discarded. (TODO: check with RobotDave on reliable messages handing of checksums)

Python bindings are super important for prototying and testing, so the middleware shall to have these too.

In our experience, synchronization is often the trickest part of writing a solid, bug free middeware stack, and the problem is greatly simplified if we just avoid needing the do synchronization by using lock-free techniqueues like co-routines.

Discovery should not cause an dangerous CPU spike when 100s or 1000s of publishers come online at boot. The simpler discovery is the better.

### Non-requirements

Many QOS policiies

- Deadline QOS: this kind of logic is easily handled on the part of the client and doesn't really belong
- Liviliness QOS: ie. notifications when someone subscribes to a topic or drops off. Having multiple methods of dealing with a loss of information creates more degrees of freedom and potential problems. We can simply use a loss in message arrival or in reliable messages not reaching as a proxy for liveliness.
- Durability QOS: After something is sent, we don't need anyone to hold on to old data. A presumed use case is that a new publisher comes online and should see data from "back in time before it came online". This is not an imporatnt use case for our systmes

Filtering packets at the middlware level. Filtering requires the middleware to be aware of the serialization, so violates some of our requirements. Additionally, its relatively trivial for a applications to do filtering by themselves, and given the requirement that our middlware prioritizes 0 copy data transfers, the cost to doing it on the client side is equivalent to doing it in the middlware.

Needing a peer-to-peer middlware: DDS is peer to peer, so it doesn't need a broker application. While this is desirable - the lack of a broker means that a failure mode is eliminiated. However, this complicates service discovery - everyone in the system must discover everyone else, so the traffic received per Node, during discovery, scales as a factorial of the number of nodes, which can be disasterous for a large system, even if discovery packets are relatively small.

### Candidates

- DDS (Cyclone with iceoryx)
- Zenoh
- Subspace
