# Microbenchmarks

`spsc_benchmark.cpp` compares LLE, Boost, and rigtorp SPSC queues using Google
Benchmark with 16, 64, 256, and 1024-byte payloads:

- throughput: one million events per iteration, at 64, 1024, and 16384 slots;
- round-trip latency (RTT): two queues of 1024 slots, 10,000 warm-up exchanges,
  then 100,000 measured request/reply exchanges per repetition.

Both use two threads and check sequence order. Neither measures process IPC or UDP.

First inspect CPUs and the allowed CPU set:

```sh
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
taskset -pc $$
```

Choose two allowed online CPUs on different physical cores (different SOCKET/CORE
pairs), preferably on the same socket and NUMA node. These IDs are examples:

```sh
export PRODUCER_CPU=0
export CONSUMER_CPU=2
```

The executable reads command-line arguments, not environment variables directly.
The shell variables below just make those arguments convenient to reuse. Both CPU
arguments are required, including when listing benchmarks.

From the repository root:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
  -DLLE_BUILD_BENCHMARKS=ON -DLLE_BUILD_APPS=OFF \
  -DLLE_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
  -DLLE_ENABLE_ASAN=OFF -DLLE_ENABLE_TSAN=OFF
cmake --build build/bench --target lle-spsc-benchmark -j4
timeout 180s ./build/bench/benchmarks/lle-spsc-benchmark \
  --producer_cpu="$PRODUCER_CPU" --consumer_cpu="$CONSUMER_CPU" \
  --benchmark_filter='^BM_(LLE|Rigtorp|Boost)<64>/1024/' \
  --benchmark_min_time=1s --benchmark_repetitions=3
```

Run all 12 RTT cases and save their results:

```sh
mkdir -p results
timeout 180s ./build/bench/benchmarks/lle-spsc-benchmark \
  --producer_cpu="$PRODUCER_CPU" --consumer_cpu="$CONSUMER_CPU" \
  --benchmark_filter='_RTT<' --benchmark_repetitions=5 \
  --benchmark_enable_random_interleaving=true \
  --benchmark_out=results/spsc-rtt.json --benchmark_out_format=json
```

For only 64-byte RTT use `--benchmark_filter='_RTT<64>'`; for only LLE use
`--benchmark_filter='^BM_LLE_RTT<'`. Quote filters containing angle brackets so
the shell does not interpret them as redirection.

Throughput `items_per_second` counts events; RTT counts complete round trips.
RTT reports `p50_ns`, `p90_ns`, `p99_ns`, and `p999_ns` (lower is better).
Each sample includes publishing a request, consumer processing and response
publication, then consuming the response. Only one request is outstanding; this
is not one-way or saturated-load latency. Setup, warm-up, joining, and sorting
are excluded from RTT timing. The main producer thread is pinned once at startup;
each consumer thread is pinned before signaling readiness. CPU IDs appear in the
console/JSON context. Pinning prevents migration, not interrupts, preemption, or
WSL host scheduling; it does not guarantee that p50 and p90 will become close.

`Time` describes a whole batch, not a single event. RTT deliberately uses one
iteration per repetition; `--benchmark_min_time` does not extend its sample batch.
Use repetitions to compare multiple batches. Their percentile counters are not
pooled percentiles across all samples.

See [the benchmark guide](../../docs/spsc-benchmarks.md) for JSON output, the full
matrix, CLion setup, timing details, and current measurement limitations.

Focused measurements for queue operations, framing, batching, and history/reorder storage belong here. Microbenchmarks do not replace end-to-end results.
