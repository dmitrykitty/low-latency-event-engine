# SPSC throughput and RTT benchmarks

## What your benchmark does

The executable compares LLE `SpscRing`, rigtorp `SPSCQueue`, and Boost
`lockfree::spsc_queue`. One producer thread pushes events and one consumer thread
reads them. They share ordinary process memory: this is not a cross-process SHM
or UDP benchmark.

Each throughput iteration transfers **1,000,000 events**. Payload sizes are
16, 64, 256, and 1024 bytes. Each queue runs at 64, 1024, and 16384 slots:
36 throughput cases. Another 12 cases measure RTT at fixed capacity 1024.
The consumer checks event sequence order. Full/empty queues are retried using
`_mm_pause()`; this is busy-waiting, not sleeping.

The code is organized as follows:

- `Record<N>` holds the metadata and payload used by the comparison queues.
- `LLEQueue`, `RigtorpQueue`, and `BoostQueue` expose the same `try_push` and
  `try_consume` interface. Consumption inspects the queued event in place.
- `run_throughput` creates the queue, starts the consumer, waits for its readiness,
  transfers the events, joins the consumer, and reports the event count.
- `BM_LLE<N>`, `BM_Rigtorp<N>`, and `BM_Boost<N>` select throughput implementations.
- `BM_LLE_RTT<N>`, `BM_Rigtorp_RTT<N>`, and `BM_Boost_RTT<N>` select RTT implementations.
- `BENCHMARK_TEMPLATE` instantiates each function for a compile-time payload size.
- `configure_rtt` selects real time and one iteration per repetition so percentile
  counters represent the complete measured batch, not only the last iteration.
- `add_capacity_size` registers capacities and selects wall-clock timing.
- `BENCHMARK_MAIN()` supplies the command-line entry point.

For throughput, queue allocation is outside the benchmark loop. Timing is paused for consumer
creation and the readiness handshake. The timed section includes signaling start,
updating event metadata, pushing, consuming, sequence validation, and joining the
consumer. Consequently this measures the complete transfer workload, not an
isolated push instruction. Google Benchmark chooses how many million-event batches
to run to meet its requested measurement duration.

### RTT workload

`run_rtt` uses a request queue and a response queue, both with 1024 slots. Only one
request is outstanding: send request, consume it on the other thread, prepare and
publish a response, consume that response on the first thread, then repeat.
The response has the same payload size and prefilled bytes, but is not a copy of
the received event. Sequence numbers are checked in both directions.

Each repetition first performs 10,000 unmeasured warm-up exchanges and then
records 100,000 RTT samples with `steady_clock`. Each sample starts immediately
before request publication and ends after response consumption/release. It includes
spinning, both queue transfers, response sequence preparation, validation, and
clock overhead. It is not one-way latency or latency under a saturated offered load.
Allocation, thread creation, warm-up, joining, and percentile sorting are outside
the timed RTT batch. Batch time additionally includes sample storage and loop work.

## Build

From the repository root, on x86-64 Linux with CMake 3.25+ and a C++23 compiler and
standard library supporting `std::expected` (for example GCC 13):

```sh
cmake -S . -B build/bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLE_BUILD_BENCHMARKS=ON \
  -DLLE_BUILD_APPS=OFF \
  -DLLE_BUILD_TESTS=OFF \
  -DBUILD_TESTING=OFF \
  -DLLE_ENABLE_ASAN=OFF \
  -DLLE_ENABLE_TSAN=OFF
cmake --build build/bench --target lle-spsc-benchmark -j4
```

Use Release without sanitizers because instrumentation and debug code change the
cost being measured. The separate build directory keeps your dev/ASan/TSan builds
unchanged. After editing benchmark code, rerun the build command.

The first configuration downloads Google Benchmark 1.9.4, Boost 1.86.0 sources
(only headers are used), and rigtorp commit
`565a5149d54930463d58cb0f69b978d439555e66`. Internet access is needed; system-wide
installation of these libraries is not. Normal builds leave benchmarks disabled.

## Run

First inspect topology and allowed CPUs:

```sh
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
taskset -pc $$
```

Choose two allowed online CPUs on different physical cores (compare SOCKET/CORE
pairs), preferably on the same socket and NUMA node. For example, only if your
topology permits CPU 0 and CPU 2:

```sh
export PRODUCER_CPU=0
export CONSUMER_CPU=2
```

The program accepts `--producer_cpu=N` and `--consumer_cpu=N`; these shell variables
are conveniences, not environment settings read by the executable. Both arguments
are required, must be different, and must name CPUs that can be selected. Malformed,
out-of-range, or unavailable selections fail instead of silently running unpinned.
The main thread is the producer; each consumer pins itself before reporting ready.
Affinity setup is outside measured work. Selected CPUs are included in result context.

List cases first:

```sh
./build/bench/benchmarks/lle-spsc-benchmark \
  --producer_cpu="$PRODUCER_CPU" --consumer_cpu="$CONSUMER_CPU" \
  --benchmark_list_tests=true
```

Run throughput for all three queues at 64-byte payload and 1024 slots:

```sh
timeout 180s ./build/bench/benchmarks/lle-spsc-benchmark \
  --producer_cpu="$PRODUCER_CPU" --consumer_cpu="$CONSUMER_CPU" \
  --benchmark_filter='^BM_(LLE|Rigtorp|Boost)<64>/1024/' \
  --benchmark_min_time=1s \
  --benchmark_repetitions=3 \
  --benchmark_enable_random_interleaving=true
```

Run only LLE at that size/capacity with `--benchmark_filter='^BM_LLE<64>/1024/'`.
Run all 36 throughput cases and save JSON:

```sh
mkdir -p results
timeout 600s ./build/bench/benchmarks/lle-spsc-benchmark \
  --producer_cpu="$PRODUCER_CPU" --consumer_cpu="$CONSUMER_CPU" \
  --benchmark_filter='^BM_(LLE|Rigtorp|Boost)<' \
  --benchmark_min_time=1s \
  --benchmark_repetitions=5 \
  --benchmark_enable_random_interleaving=true \
  --benchmark_out=results/spsc-throughput.json \
  --benchmark_out_format=json
```

`--benchmark_min_time=1s` asks the framework to calibrate enough batches for about
one second of measured throughput work per repetition. It is not the runtime of the whole
command. Repetitions show variation; random interleaving reduces ordering bias.
`timeout` prevents an indefinite hang if a transfer stops making progress; increase
it on slower systems. Incomplete or error-marked runs are not usable measurements.

**The previous `lle/throughput/...` filters and `LLE_BENCH_*_CPU` environment
variables no longer apply.** Your simplified executable does not read those
variables. Names now look like `BM_LLE<64>/1024/real_time`.

Run all 12 RTT cases:

```sh
mkdir -p results
timeout 180s ./build/bench/benchmarks/lle-spsc-benchmark \
  --producer_cpu="$PRODUCER_CPU" --consumer_cpu="$CONSUMER_CPU" \
  --benchmark_filter='_RTT<' \
  --benchmark_repetitions=5 \
  --benchmark_enable_random_interleaving=true \
  --benchmark_out=results/spsc-rtt.json --benchmark_out_format=json
```

Use `--benchmark_filter='_RTT<64>'` for all queues with a 64-byte payload or
`--benchmark_filter='^BM_LLE_RTT<'` for LLE at all four sizes. RTT has one fixed
100,000-sample iteration per repetition, so `--benchmark_min_time` does not extend
it. Do not override its iteration count: counters would describe only the last
iteration. Omitting the filter runs all 48 throughput and RTT cases.

## Read the output

- Throughput `items_per_second` is completed events per second. `15M/s` means 15
  million events/s. RTT counts round trips instead. Compare matching cases.
- Throughput `Time` is average wall time for **one million-event batch**, not one event.
  For example 50,000,000 ns is 50 ms per batch, or 20M events/s. This is only an
  example, not a measured result.
- `Iterations` is the number of those batches, not the number of events.
- `CPU` is not the combined CPU cost of both threads; use wall time and throughput
  for this comparison.
- `_median`, `_mean`, `_stddev`, and `_cv` summarize repetitions. Look at the
  individual results and spread as well as the median.

RTT `Time` describes the 100,000-exchange measured batch. `p50_ns`, `p90_ns`,
`p99_ns`, and `p999_ns` describe individual RTT samples in nanoseconds; lower is
better. For example p99 means at least 99% of samples are no greater than that
value. `p999_ns` is p99.9, not p99.99. Console counters can use SI suffixes.
Repetition summaries aggregate per-run percentiles, not pooled raw observations.
Raw samples are not exported. Dividing throughput batch time by event count is
not an individual event latency measurement. Older harness results are not directly
comparable because workload and validation have changed.

## Current limitations

This is a simpler learning benchmark, not yet a controlled final performance study:

- Throughput has no explicit warm-up or complete payload pre-touch. RTT does have
  its 10,000-exchange warm-up. Framework calibration
  is not a documented steady-state warm-up phase.
- Both threads are pinned, but CPUs are not reserved or isolated. Interrupts,
  preemption, competing workloads, and WSL host scheduling can still affect tails.
  Pinning does not guarantee a small p50-to-p90 gap. Distinct logical CPU IDs may
  still be SMT siblings; use topology to choose different physical cores.
- Only sequence order is checked, not payload bytes, timestamp, stream, or length.
  This is not a replacement for the correctness tests. Baseline adapters expose
  the fixed-size payload array and do not validate the stored `length` field.
- LLE errors other than temporary full/empty conditions are collapsed into `false`
  by the adapter. An unexpected permanent error can cause an endless retry; use
  the timeout above. A future improvement is separate fatal-error reporting.
- Queue contracts and memory footprints differ. In this record layout the baseline
  contains 24 bytes of metadata plus 64 bytes of payload; LLE uses a 128-byte slot
  for the same payload. LLE also performs additional state/layout-related checks.
- Joining the consumer is included in throughput timing, but excluded for RTT. Thread creation is excluded, but a
  new consumer is created for each million-event batch.

For final comparisons prefer native Linux, close competing workloads, keep power
settings consistent, and record compiler, flags, kernel, CPU, topology, and commit
alongside JSON. WSL runs are useful preliminary checks but should be labeled as such.
No winner can be inferred from this revised harness until it is measured.

## CLion

Create a Release CMake profile using `build/bench` and the `-D` options from the
build command. Reload CMake and select `lle-spsc-benchmark` as the run target.
Put `--benchmark_filter=^BM_LLE<64>/1024/ --benchmark_min_time=1s` in **Program
arguments**, not CMake options. Also add `--producer_cpu=0 --consumer_cpu=2`,
replacing these examples with your chosen CPU IDs. Use literal numbers in CLion;
shell variable expansion in the terminal examples is performed by the shell.
Run without the debugger. For relative JSON paths, set the working directory to
the repository root and create `results` first.
For RTT, use `--benchmark_filter=^BM_LLE_RTT< --benchmark_repetitions=5` instead.
