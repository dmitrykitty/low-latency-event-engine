# Low-Latency Event Distribution Engine

A C++23/Linux transport for moving small opaque binary events between processes and hosts, with an emphasis on
predictable tail latency, explicit backpressure, and measurable delivery guarantees.

## Performance

The shared-memory SPSC ring at the core of the engine was benchmarked against
[rigtorp::SPSCQueue](https://github.com/rigtorp/SPSCQueue) and
[Boost.Lockfree `spsc_queue`](https://www.boost.org/doc/libs/release/doc/html/lockfree.html)
under identical conditions. At 16–256-byte payloads **LLE has the lowest ping-pong
latency** (p50 7–8% below rigtorp and 9–14% below Boost); at 1 KB the three are level.
**Throughput is 5.2× rigtorp's at 16 bytes, 2.7× at 256 bytes, and 1.5× at 1 KB**;
rigtorp is ahead at 64 bytes.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/performance/latency-p50-dark.svg">
  <img alt="Ping-pong p50 latency per process for LLE, rigtorp and Boost at 16, 64, 256 and 1024 bytes" src="figures/performance/latency-p50-light.svg">
</picture>

Each small dot above is one benchmark process with its own random memory layout.
LLE's dots stay in a tight cluster, while rigtorp's split into a fast and a slow
group depending on where its queue object lands; that layout effect, and how the ring
was made immune to it, is described in the performance report.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/performance/throughput-1024-dark.svg">
  <img alt="Throughput of LLE, rigtorp and Boost with a 1024-slot queue at 16, 64, 256 and 1024 bytes" src="figures/performance/throughput-1024-light.svg">
</picture>

| Payload | p50 round trip, ns<br>LLE / rigtorp / Boost | p99 round trip, ns<br>LLE / rigtorp / Boost | Throughput, M events/s<br>LLE / rigtorp / Boost |
|---:|---:|---:|---:|
| 16 B | **304** / 328 / 353 | **355** / 404 / 417 | **96.0** / 18.6 / 14.2 |
| 64 B | **366** / 399 / 408 | **424** / 459 / 455 | 14.7 / **15.7** / 13.4 |
| 256 B | **527** / 569 / 578 | 624 / 637 / 637 | **26.9** / 10.0 / 9.5 |
| 1 KB | 893 / 881 / 889 | 962 / 969 / 971 | **13.4** / 9.0 / 7.0 |

Means over 32 latency processes and 10 throughput processes per case; bold marks the
best value where its 95% confidence interval does not overlap the others. Measured on
an Intel Core i7-10750H laptop with the benchmark threads on isolated, tickless cores
at a fixed 2.6 GHz; round trips are not halved. Method, confidence intervals, tail
percentiles, caveats, and the exact command are in the
[performance report](docs/performance.md); the raw dataset is in
[results/representative](results/representative/2026-10-16-spsc/).

Design choices whose effect was measured in A/B runs
([engineering record](docs/performance.md#engineering-record-spsc-ring-hot-path)):
each handle caches the other side's cursor instead of reading it on every operation,
the per-event hot path is inline in the header, handles are cache-line aligned, and
ring memory starts on a 128-byte boundary so the producer and consumer cursors never
share a prefetched line pair.

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
    cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -E 'SeparateMapping|SeparateProcesses'

See [testing instructions](docs/testing.md) for integration tests, sanitizer coverage, and the
TSan launcher required when running directly from CLion.

## Implementation phase I

For the SPSC queue benchmarks (ping-pong latency and throughput against Boost and
rigtorp), see the [benchmark guide](benchmarks/micro/README.md) and the recorded,
low-noise runner in [scripts/bench](scripts/bench/README.md).

Implemented:

- POSIX shared-memory creation, attachment, mapping, close, and owner-controlled unlink.
- Versioned ring layout, checked initialization, and attachment validation.
- Fixed-capacity SPSC publication, acquisition, explicit release, and publication closure.
- Release/acquire synchronization, full/empty detection, slot reuse, and draining after closure.
- GoogleTest unit tests, threaded stress testing, and a two-process test transferring 10 million events.
- Single-event LLEP DATA encoding/decoding and reusable UDP transport.
- A library sender: ingress SHM -> one DATA packet -> ingress slot release -> serial UDP fan-out.
  See [sender behavior and usage](docs/architecture.md#basic-sender-loop).
- A basic library receiver: UDP -> DATA validation -> egress SHM, with malformed
  packet accounting, explicit drops, and idle shutdown.
  See [receiver behavior and usage](docs/architecture.md#basic-receiver-loop).
- A local producer -> sender -> receiver -> consumer integration test.
- Packet sequencing with exact missing ranges, gap counters, and duplicate/old-packet suppression.
- Byte-identical fan-out to 1, 2, and 4 destinations, including the full path through receiver processes.

The shared-memory ring implementation is complete under the documented single-producer/single-consumer
and startup assumptions. The basic library data path supports one event per packet
and serial-unicast fan-out to multiple BestEffort receivers. Sender and receiver
executables remain scaffolding. End-to-end (SHM -> UDP -> SHM) benchmarks are the next step.

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
- **lle-spsc-latency**: optional Release ping-pong latency benchmark (rdtsc, all percentiles), same option.
