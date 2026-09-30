# Low-Latency Event Distribution Engine

A C++23/Linux transport for moving small opaque binary events between processes and hosts, with an emphasis on
predictable tail latency, explicit backpressure, and measurable delivery guarantees.

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
- A basic library sender: ingress SHM -> one DATA packet -> UDP -> ingress slot release.
  See [sender behavior and usage](docs/architecture.md#basic-sender-loop).

The shared-memory ring implementation is complete under the documented single-producer/single-consumer
and startup assumptions. The receiver loop and complete end-to-end delivery remain unfinished; sender and
receiver executables are scaffolding. The next task is the receiver loop into egress SHM.

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
- **lle-smoke-test**: minimal CTest target validating the toolchain and public types.
- **lle-shm-segment-test**: shared-memory lifecycle tests;
- **lle-ring-init-test**: ring layout, initialization, and attachment tests;
- **lle-ring-transfer-test**: publication, acquisition, release, closure, and boundary tests;
- **lle-ring-concurrency-test**: thread and process integration tests;
- **lle-sender-test**: ingress SHM to UDP sender integration and failure handling.
