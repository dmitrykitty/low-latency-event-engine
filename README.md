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

Implemented:

- POSIX shared-memory creation, attachment, mapping, close, and owner-controlled unlink.
- Versioned ring layout, checked initialization, and attachment validation.
- Fixed-capacity SPSC publication, acquisition, explicit release, and publication closure.
- Release/acquire synchronization, full/empty detection, slot reuse, and draining after closure.
- GoogleTest unit tests, threaded stress testing, and a two-process test transferring 10 million events.

The shared-memory ring implementation is complete under the documented single-producer/single-consumer
and startup assumptions. UDP transport and end-to-end event delivery remain unfinished; sender and
receiver executables are scaffolding. The next task is the two-host UDP smoke test.

Latest verification: 67 development tests, 31 ring tests under ASan/UBSan, and 66 in-process tests
under TSan passed. These are correctness checks, not latency benchmarks.

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
- **lle-ring-concurrency-test**: thread and process integration tests.

