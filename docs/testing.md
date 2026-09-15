# Running tests

Run commands in WSL from the repository root:

```bash
cd /home/dzmitry-kitty/projects/event_engine/event_distribution_engine
```

CMake uses an installed GoogleTest package or downloads version 1.17.0. Tests require both
`BUILD_TESTING=ON` and `LLE_BUILD_TESTS=ON`; the development and sanitizer presets enable them.
Build before running CTest: building also discovers the individual GoogleTest cases.

## Development and integration tests

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

Run only the two concurrency/integration tests:

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
ctest --preset asan -R 'lle.shm.(ring|transfer|concurrent)' --output-on-failure
```

This checks ring unit tests and both integration tests for memory errors and undefined behavior.
These sanitizers do not replace a data-race detector.

## ThreadSanitizer

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -E SeparateMapping --output-on-failure
```

For just the threaded stress test:

```bash
ctest --preset tsan -R ThreadsTransfer --output-on-failure
```

We exclude the separate-process test because TSan tracks threads within a process; it does not provide
cross-process shared-memory race verification. Use the threaded equivalent for race detection and
development/ASan runs for process integration.

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

