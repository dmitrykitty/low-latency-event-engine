# Microbenchmarks

`spsc_benchmark.cpp` compares LLE, Boost, and rigtorp SPSC queues using Google
Benchmark. It measures throughput with 64-byte payloads at 64, 1024, and 16384
slots. Each iteration transfers one million events between two threads and checks
their sequence order. It does not measure RTT, latency percentiles, or process IPC.

From the repository root:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
  -DLLE_BUILD_BENCHMARKS=ON -DLLE_BUILD_APPS=OFF \
  -DLLE_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
  -DLLE_ENABLE_ASAN=OFF -DLLE_ENABLE_TSAN=OFF
cmake --build build/bench --target lle-spsc-benchmark -j4
timeout 180s ./build/bench/benchmarks/lle-spsc-benchmark \
  --benchmark_filter='^BM_(LLE|Rigtorp|Boost)_64/1024/' \
  --benchmark_min_time=1s --benchmark_repetitions=3
```

Compare `items_per_second` (higher is better). `Time` is per million-event batch,
not per event. No CPU environment variables are used by this version.

See [the benchmark guide](../../docs/spsc-benchmarks.md) for JSON output, the full
matrix, CLion setup, timing details, and current measurement limitations.

Focused measurements for queue operations, framing, batching, and history/reorder storage belong here. Microbenchmarks do not replace end-to-end results.
