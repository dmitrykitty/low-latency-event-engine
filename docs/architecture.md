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

