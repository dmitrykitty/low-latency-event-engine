# Low-Latency Event Distribution Engine

A C++23/Linux transport for moving small opaque binary events between processes and hosts, with an emphasis on
predictable tail latency, explicit backpressure, and measurable delivery guarantees.

## Command-line build

    cmake --preset dev
    cmake --build --preset dev
    ctest --preset dev

Sanitizer configurations use separate build trees:

    cmake --preset asan && cmake --build --preset asan && ctest --preset asan
    cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan

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


