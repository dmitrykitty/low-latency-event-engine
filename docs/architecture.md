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

## Basic sender loop

`Sender::initialize(config)` attaches to an existing, initialized ingress segment,
attaches its consumer ring handle, parses one numeric IPv4 destination, opens the
UDP socket, and chooses a nonzero random session ID. Startup failures return
`InitError`. The external producer owns initialization and unlinking of ingress SHM.
The sender owns its attachment and socket through the existing resource classes.

The loop is deliberately small:

```text
try_acquire -> encode one DATA packet -> send_to -> release
```

`run()` reuses an aligned, fixed-size packet buffer. The configured datagram limit
bounds the encoder's output, including padding. Event metadata and payload bytes
come directly from the acquired ring view; the view remains valid until release.
Packet sequences start at 1 and advance for each encoded packet, even if sending
fails. Invalid events do not consume a packet sequence. The sender stops before
packet sequence overflow; a new sender instance starts a new session.

BestEffort consumes and counts events that fail encoding or sending, then continues
with the next slot. This prevents an unsendable event from blocking the queue.
`events_in` counts acquired events; `events_out` and `packets_sent` count successful
socket sends, which do not guarantee remote delivery. `events_dropped`,
`encode_errors`, and `send_errors` record failed attempts. An invalid ring slot
increments `ingress_errors` and stops the loop because it cannot be safely acquired.

An empty ring is polled continuously when `busy_spin_ingress` is true. When false,
the loop sleeps for 1 ms between empty polls, trading latency for lower CPU use.
Closing publication drains queued events before `run()` returns. Calling
`request_stop()` from another thread finishes any acquired event and leaves the
remaining queue untouched. Read `stats()` after `run()` returns, joining its worker
thread first. Moving or destroying the sender also requires that worker to be stopped.

This milestone supports one destination and BestEffort delivery with `cpu == -1`.
Initialization rejects unsupported settings. Receiver processing, fan-out, affinity,
and loss recovery remain later plan steps. The command-line sender is still a scaffold;
the working sender loop is available through the library API.

Example, after the external producer has created and initialized `/my-ingress`:

```cpp
lle::SenderConfig config;
config.ingress_shm_name = "/my-ingress";
config.receivers.push_back({"127.0.0.1", 9000});
auto sender = lle::Sender::initialize(config);
if (!sender) {
    // Report sender.error() during startup.
    return;
}
sender->run(); // Returns after ingress closes and drains, or request_stop().
const auto stats = sender->stats();
```
