# Low-Latency Event Distribution Engine

A C++23/Linux transport for moving small opaque binary events between processes and hosts, with an emphasis on
predictable tail latency, explicit backpressure, and measurable delivery guarantees.

## Architecture guide

Start with the [component map](docs/architecture.md#current-data-path), then follow
[one event in the sequence diagram](docs/architecture.md#one-event-from-producer-to-consumer).
They show how producer, ingress, Sender, Receiver, egress, and consumer connect,
where payloads are copied, and when ring slots can be reused. The
[ownership guide](docs/architecture.md#resource-and-thread-ownership) explains who
creates and unlinks SHM and how to stop the worker threads.

The [data layout walkthrough](docs/data-flow.md) shows how `EventView` becomes a
SHM slot and then an LLEP packet, including headers, field offsets, and an example
with exact encoded bytes.

## Command-line build

    cmake --preset dev
    cmake --build --preset dev
    ctest --preset dev

Sanitizer configurations use separate build trees:

    cmake --preset asan && cmake --build --preset asan && ctest --preset asan
    cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -E SeparateMapping

See [testing instructions](docs/testing.md) for integration tests, sanitizer coverage, and the
TSan launcher required when running directly from CLion.

## Implementation phase I

For the optional Google Benchmark comparison of LLE, Boost, and rigtorp queues,
see [SPSC benchmark instructions](benchmarks/micro/README.md).

Implemented:

- POSIX shared-memory creation, attachment, mapping, close, and owner-controlled unlink.
- Versioned ring layout, checked initialization, and attachment validation.
- Fixed-capacity SPSC publication, acquisition, explicit release, and publication closure.
- Release/acquire synchronization, full/empty detection, slot reuse, and draining after closure.
- GoogleTest unit tests, threaded stress testing, and a two-process test transferring 10 million events.
- Single-event LLEP DATA encoding/decoding and reusable UDP transport.
- A basic library sender: ingress SHM -> one DATA packet -> ingress slot release -> UDP.
  See [sender behavior and usage](docs/architecture.md#basic-sender-loop).
- A basic library receiver: UDP -> DATA validation -> egress SHM, with malformed
  packet accounting, explicit drops, and idle shutdown.
  See [receiver behavior and usage](docs/architecture.md#basic-receiver-loop).
- A local producer -> sender -> receiver -> consumer integration test.
- Packet sequencing with exact missing ranges, gap counters, and duplicate/old-packet suppression.

The shared-memory ring implementation is complete under the documented single-producer/single-consumer
and startup assumptions. The basic library data path supports one event per packet
and one BestEffort receiver. Sender and receiver executables remain scaffolding.
Serial-unicast fan-out is the next protocol-path step.

See [testing instructions](docs/testing.md) for development and sanitizer checks.
These are correctness checks, not latency benchmarks.

## Planned data path

    external producer
        -> POSIX shared-memory SPSC ingress
        -> framing, sequencing, opportunistic batching
        -> UDP serial-unicast fan-out
        -> parsing, gap detection, optional ordered recovery
        -> POSIX shared-memory SPSC egress
        -> external consumer

The public API, shared-memory layout, and wire protocol are separate contracts. The transport remains payload-agnostic;
challenge-specific integration belongs under **adapters/**.

## Current targets

- **lle**: static library and public headers;
- **lle-sender**: sender process entry point;
- **lle-receiver**: receiver process entry point;
- **lle-udp-smoke**: standalone UDP smoke program;
- **lle-smoke-test**: minimal CTest target validating the toolchain and public types.
- **lle-byte-io-test**, **lle-encoder-test**, **lle-decoder-test**: single-event protocol unit tests;
- **lle-shm-segment-test**: shared-memory lifecycle tests;
- **lle-ring-init-test**: ring layout, initialization, and attachment tests;
- **lle-ring-transfer-test**: publication, acquisition, release, closure, and boundary tests;
- **lle-ring-concurrency-test**: thread and process integration tests;
- **lle-udp-transport-test**: loopback UDP and protocol transport integration;
- **lle-sender-test**: ingress SHM to UDP sender integration and failure handling.
- **lle-receiver-test**: UDP to egress SHM receiver integration and the complete local data path.
- **lle-spsc-benchmark**: optional Release throughput/RTT comparison, enabled with `LLE_BUILD_BENCHMARKS=ON`.
