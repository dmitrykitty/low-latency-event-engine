# Architecture

## Boundary

The engine owns shared-memory ingress/egress, transport framing, UDP fan-out, explicit backpressure, metrics, and optional bounded recovery. External producers and consumers own application semantics.

## Planned components

- POSIX shared-memory segment lifecycle;
- fixed-capacity SPSC ingress and egress;
- packet/event sequencing and versioned framing;
- opportunistic batcher and serial-unicast UDP sender;
- defensive UDP parser and egress publisher;
- optional NACK, retransmission history, and bounded reorder window.

Detailed component and thread ownership diagrams will be added as each milestone is implemented.

## Hot-path implementation constraints

The LLEP wire layout permits allocation-free processing; the engine makes that an implementation requirement. After initialization, the steady-state packet encoder and decoder MUST NOT dynamically allocate while processing packets or events. Packet buffers, decoded-view storage, and other bounded working memory are allocated before entering the hot path and then reused.

Transmit and receive packet buffers MUST begin at an address aligned to at least 8 bytes. For fixed local buffers, one suitable declaration is:

```cpp
alignas(8) std::array<std::byte, MAX_LLEP_PACKET_BYTES> buffer;
```

This base alignment allows the naturally aligned offsets defined by LLEP to produce aligned integer addresses. Decoders still use explicit byte-order helpers and MUST NOT rely on casting packet bytes to native C++ structures.
