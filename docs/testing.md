# Running tests

Run commands in WSL from the repository root:

```bash
cd /home/dzmitry-kitty/projects/event_engine/event_distribution_engine
```

CMake uses an installed GoogleTest package or downloads version 1.17.0. Tests require both
`BUILD_TESTING=ON` and `LLE_BUILD_TESTS=ON`; the development and sanitizer presets enable them.
Build before running CTest: building also discovers the individual GoogleTest cases.

## Test map

Use `ctest --preset dev -R '<filter>' --output-on-failure` to run one group after
building. Replace `dev` with `asan` or `tsan` for the matching sanitizer build.
For TSan, also add `-E 'SeparateMapping|SeparateProcesses'` to exclude the cross-process tests described
below.

| Filter | Coverage | Source |
|--------|----------|--------|
| `lle.protocol.` | Byte helpers and single-event DATA encoding/decoding | [unit tests](../tests/unit/) |
| `lle.reliability.gap.` | Exact missing ranges, duplicate/old packets, session isolation, and maximum sequence | [gap_detector_test.cpp](../tests/unit/gap_detector_test.cpp) |
| `lle.shm.(segment|ring|transfer).` | SHM lifecycle, ring layout, publication, acquisition, and closure | [unit tests](../tests/unit/) |
| `lle.shm.concurrent.` | Threaded and two-process SPSC integrity tests | [ring_concurrency_test.cpp](../tests/integration/ring_concurrency_test.cpp) |
| `lle.transport.` | Loopback UDP, protocol round trip, truncation, and address errors | [udp_transport_test.cpp](../tests/integration/udp_transport_test.cpp) |
| `lle.sender.` | Ingress SHM to UDP and sender failure handling | [sender_test.cpp](../tests/integration/sender_test.cpp) |
| `lle.receiver.` | UDP to egress SHM, gap/duplicate/session counters, suppression of repeat delivery, and the complete local producer-to-consumer path | [receiver_test.cpp](../tests/integration/receiver_test.cpp) |
| `lle.fanout.` | Full path through 1, 2, and 4 receiver processes and their separate egress rings | [fanout_process_test.cpp](../tests/integration/fanout_process_test.cpp) |

These are correctness checks. Two-host full-path delivery, fan-out performance, recovery,
parser fuzzing, and end-to-end latency measurements remain separate work.
For the data path and ownership rules, see the [architecture guide](architecture.md).

## Development and integration tests

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

Run only the two SHM concurrency tests:

```bash
ctest --preset dev -R 'lle.shm.concurrent.' --output-on-failure
```

Run only the two-process test:

```bash
ctest --preset dev -R SeparateMapping --output-on-failure
```

`ThreadsTransferOrderedEventsAndObserveFinalClosure` transfers 200,000 events between two threads.
`SeparateMappingTransfersTenMillionEvents` transfers 10 million events between a parent and child
process. The child closes the inherited mapping, opens the segment by name, and attaches its own ring
handle. Forking after initialization provides startup ordering for this test; an independently launched
application still needs an initialization handshake.

Both tests verify event sequence, stream ID, timestamp, payload contents, and final closure. A 64-slot
ring forces repeated slot reuse. Each transfer has a 45-second deadline; CTest enforces a 60-second
test timeout. Timeout is a test failure, not a throughput measurement.

## ASan and UBSan

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

The full suite includes protocol, SHM, UDP transport, sender, receiver, and process fan-out tests.
To run only the ring unit tests and both SHM concurrency tests:

```bash
ctest --preset asan -R 'lle.shm.(ring|transfer|concurrent)' --output-on-failure
```

ASan/UBSan check for memory errors and undefined behavior.
These sanitizers do not replace a data-race detector.

## ThreadSanitizer

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -E 'SeparateMapping|SeparateProcesses' --output-on-failure
```

For just the threaded stress test:

```bash
ctest --preset tsan -R ThreadsTransfer --output-on-failure
```

We exclude the separate-process SHM and fan-out tests because TSan tracks threads within a
process; it does not provide cross-process shared-memory race verification. Use the
threaded ring and sender tests for race detection and development/ASan runs for process integration.

### Why the launcher is necessary here

On the tested WSL environment, even a minimal program built with GCC 13 or 14 failed before `main()`:

```text
FATAL: ThreadSanitizer: unexpected memory mapping
```

TSan needs specific virtual-address ranges for its bookkeeping. The randomized process layout
conflicted with those ranges. Running through `setarch x86_64 -R` resolved the startup failure.
This disables address randomization only for the launched process and its children; it does not change
system-wide settings or turn off TSan's race detection.

`tests/CMakeLists.txt` applies this launcher to Linux TSan test discovery and CTest execution. CMake
3.29 and newer use `TEST_LAUNCHER`; older supported versions use `CROSSCOMPILING_EMULATOR` for this
launching role. The tested environment permits `setarch -R`; environments that prohibit it may need
a compatible sanitizer runtime or host configuration.

### CLion's direct GoogleTest runner

Reload CMake after changing test registration. Select the matching WSL toolchain and CMake profile.
CLion's green GoogleTest triangle may run the executable directly, bypassing the CTest launcher.
If the command starts with `build/tsan/tests/lle-ring-concurrency-test`, the workaround is missing.

Use the CTest command above in CLion's WSL terminal, or run the executable explicitly:

```bash
setarch x86_64 -R \
  ./build/tsan/tests/lle-ring-concurrency-test \
  --gtest_filter='RingConcurrencyTest.ThreadsTransfer*'
```

An exit code of 66 with `unexpected memory mapping` is a startup failure. A TSan data-race report
instead identifies conflicting accesses and their stack traces. Inspect the diagnostic, not just the
exit code.


## Sender integration tests

```bash
ctest --preset dev -R 'lle.sender.' --output-on-failure
ctest --preset asan -R 'lle.sender.' --output-on-failure
ctest --preset tsan -R 'lle.sender.' --output-on-failure
```

Build the matching preset first. `lle-sender-test` creates real POSIX ingress SHM,
uses `Sender` to send over loopback UDP, and decodes received DATA packets. It checks
metadata and binary payload preservation, empty/maximum payloads, FIFO ordering,
slot reuse across wraps, closure, stop requests, configured datagram limits, startup
errors, malformed slots, and encoding/send failures. The send failure test uses a
broadcast address without socket broadcast permission, so Linux rejects the send
without transmitting a packet. Test worker threads are stopped and joined before
reading counters or cleaning up shared memory.

The parameterized sender fan-out tests compare complete UDP datagrams for 1, 2,
and 4 destinations, including empty and maximum payloads, stable session identity,
and shared packet sequences. Separate cases verify partial success, continuing
past a failed destination, total send failure, and one encoding failure per event.

`lle-fanout-process-test` runs the complete ingress -> Sender -> UDP -> Receiver ->
egress path with 1, 2, and 4 child receiver processes. Sockets and rings are initialized
before fork so all endpoints are ready when Sender starts; workers start only after
fork. Each child independently attaches its egress consumer, checks both events,
stops and joins its receiver, and reports success through its exit status. The
parent reaps children before releasing resources. Each child has a 3-second receive
deadline; CTest bounds the whole case to 10 seconds. These checks verify correctness,
without measuring fan-out latency or simulating independently launched hosts.

## Receiver integration tests

```bash
ctest --preset dev -R 'lle.receiver.' --output-on-failure
ctest --preset asan -R 'lle.receiver.' --output-on-failure
ctest --preset tsan -R 'lle.receiver.' --output-on-failure
```

Build the matching preset first. `lle-receiver-test` sends real loopback UDP DATA
packets and consumes events through a separate egress SHM attachment. It checks
metadata and binary payload preservation, FIFO order and slot reuse, empty/maximum
payloads, malformed/oversized packets, full egress and small payload capacity,
gap-range versus missing-packet counts, duplicate/old-packet suppression, foreign
sessions, sequence tracking after an egress failure,
closure, idle stop, move ownership, unlinking, bind failures, and invalid settings.
It also checks the complete local producer -> sender -> receiver -> consumer path.
All counter reads occur after stopping and joining the worker.
