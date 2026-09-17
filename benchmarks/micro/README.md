# Microbenchmarks

`spsc_benchmark.cpp` compares LLE, Boost, and rigtorp SPSC queues using Google
Benchmark. See [the benchmark guide](../../docs/spsc-benchmarks.md) for workload
definitions, native Linux and CLion instructions, and result interpretation.

Focused measurements for queue operations, framing, batching, and history/reorder storage belong here. Microbenchmarks do not replace end-to-end results.
