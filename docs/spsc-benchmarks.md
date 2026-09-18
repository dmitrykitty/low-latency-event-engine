# SPSC throughput benchmark

## What your benchmark does

The executable compares LLE `SpscRing`, rigtorp `SPSCQueue`, and Boost
`lockfree::spsc_queue`. One producer thread pushes events and one consumer thread
reads them. They share ordinary process memory: this is not a cross-process SHM
or UDP benchmark.

Each Google Benchmark iteration transfers **1,000,000 events with 64-byte payloads**.
Each queue runs at 64, 1024, and 16384 slots: nine cases in total.
The consumer checks event sequence order. Full/empty queues are retried using
`_mm_pause()`; this is busy-waiting, not sleeping.

The code is organized as follows:

- `Record<N>` holds the metadata and payload used by the comparison queues.
- `LLEQueue`, `RigtorpQueue`, and `BoostQueue` expose the same `try_push` and
  `try_consume` interface. Consumption inspects the queued event in place.
- `run_throughput` creates the queue, starts the consumer, waits for its readiness,
  transfers the events, joins the consumer, and reports the event count.
- `BM_LLE_64`, `BM_Rigtorp_64`, and `BM_Boost_64` select the queue implementation.
- `add_capacity_size` registers capacities and selects wall-clock timing.
- `BENCHMARK_MAIN()` supplies the command-line entry point.

Queue allocation is outside the benchmark loop. Timing is paused for consumer
creation and the readiness handshake. The timed section includes signaling start,
updating event metadata, pushing, consuming, sequence validation, and joining the
consumer. Consequently this measures the complete transfer workload, not an
isolated push instruction. Google Benchmark chooses how many million-event batches
to run to meet its requested measurement duration.

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

List cases first:

```sh
./build/bench/benchmarks/lle-spsc-benchmark --benchmark_list_tests=true
```

Run all three queues at 1024 slots:

```sh
timeout 180s ./build/bench/benchmarks/lle-spsc-benchmark \
  --benchmark_filter='^BM_(LLE|Rigtorp|Boost)_64/1024/' \
  --benchmark_min_time=1s \
  --benchmark_repetitions=3 \
  --benchmark_enable_random_interleaving=true
```

Run only LLE at that capacity with `--benchmark_filter='^BM_LLE_64/1024/'`.
Run the complete nine-case comparison and save JSON:

```sh
mkdir -p results
timeout 600s ./build/bench/benchmarks/lle-spsc-benchmark \
  --benchmark_min_time=1s \
  --benchmark_repetitions=5 \
  --benchmark_enable_random_interleaving=true \
  --benchmark_out=results/spsc-throughput.json \
  --benchmark_out_format=json
```

`--benchmark_min_time=1s` asks the framework to calibrate enough batches for about
one second of measured work per repetition. It is not the runtime of the whole
command. Repetitions show variation; random interleaving reduces ordering bias.
`timeout` prevents an indefinite hang if a transfer stops making progress; increase
it on slower systems. Incomplete or error-marked runs are not usable measurements.

**The previous `lle/throughput/...` filters and `LLE_BENCH_*_CPU` environment
variables no longer apply.** Your simplified executable does not read those
variables. The registered names now look like `BM_LLE_64/1024/real_time`.

## Read the output

- `items_per_second` is completed events per second. `15M/s` means 15 million
  events/s. Compare the same capacity; higher is better.
- `Time` is average wall time for **one million-event batch**, not one event.
  For example 50,000,000 ns is 50 ms per batch, or 20M events/s. This is only an
  example, not a measured result.
- `Iterations` is the number of those batches, not the number of events.
- `CPU` is not the combined CPU cost of both threads; use wall time and throughput
  for this comparison.
- `_median`, `_mean`, `_stddev`, and `_cv` summarize repetitions. Look at the
  individual results and spread as well as the median.

This version measures **no RTT or latency percentiles**. Dividing batch time by
event count gives an amortized time per event, not the latency experienced by an
individual event. Earlier results from the old harness are not directly comparable:
the workload and validation have changed.

## Current limitations

This is a simpler learning benchmark, not yet a controlled final performance study:

- There is no explicit warm-up or complete payload pre-touch. Framework calibration
  is not a documented steady-state warm-up phase.
- Threads are not individually pinned. On native Linux, inspect topology with
  `lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE`. You can optionally prefix a run with
  `taskset -c 0,2` if those CPUs are allowed and on different physical cores, but
  this only restricts both threads to a shared CPU set; it does not assign one
  thread to each core or prevent migration between them.
- Only sequence order is checked, not payload bytes, timestamp, stream, or length.
  This is not a replacement for the correctness tests. Baseline adapters expose
  the fixed-size payload array and do not validate the stored `length` field.
- LLE errors other than temporary full/empty conditions are collapsed into `false`
  by the adapter. An unexpected permanent error can cause an endless retry; use
  the timeout above. A future improvement is separate fatal-error reporting.
- Queue contracts and memory footprints differ. In this record layout the baseline
  contains 24 bytes of metadata plus 64 bytes of payload; LLE uses a 128-byte slot
  for the same payload. LLE also performs additional state/layout-related checks.
- Joining the consumer is included in timing. Thread creation is excluded, but a
  new consumer is created for each million-event batch.

For final comparisons prefer native Linux, close competing workloads, keep power
settings consistent, and record compiler, flags, kernel, CPU, topology, and commit
alongside JSON. WSL runs are useful preliminary checks but should be labeled as such.
No winner can be inferred from this revised harness until it is measured.

## CLion

Create a Release CMake profile using `build/bench` and the `-D` options from the
build command. Reload CMake and select `lle-spsc-benchmark` as the run target.
Put `--benchmark_filter=^BM_LLE_64/1024/ --benchmark_min_time=1s` in **Program
arguments**, not CMake options. No CPU environment variables are required.
Run without the debugger. For relative JSON paths, set the working directory to
the repository root and create `results` first.
