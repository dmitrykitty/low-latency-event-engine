# Stress tests

This directory is reserved for additional long-running stress workloads. The
implemented SPSC stress tests currently live in
[ring_concurrency_test.cpp](../integration/ring_concurrency_test.cpp):

- `ThreadsTransferOrderedEventsAndObserveFinalClosure` transfers 200,000 events
  between two threads and is suitable for TSan.
- `SeparateMappingTransfersTenMillionEvents` transfers 10 million events between
  parent and child processes using separate SHM mappings. Run it under development
  or ASan/UBSan builds; TSan does not verify cross-process SHM races.

Both check event metadata, payload contents, order, slot reuse, and final closure.
From the repository root, after building the development preset:

```bash
ctest --preset dev -R 'lle.shm.concurrent.' --output-on-failure
```

See [testing instructions](../../docs/testing.md) for sanitizer commands and
deadlines. These tests check integrity; they do not measure throughput or latency.

