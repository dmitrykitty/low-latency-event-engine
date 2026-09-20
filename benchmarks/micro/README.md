# SPSC throughput and RTT benchmarks

This is the canonical benchmark guide. The runner is [run.sh](../run.sh) and the
implementation is [micro/spsc_benchmark.cpp](spsc_benchmark.cpp).

## Quick start: run and save all cases

Inspect CPU topology and your allowed CPU set first:

```sh
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
taskset -pc $$
```

Select different physical cores, preferably on the same socket and NUMA node.
CPU 0 and CPU 2 below are examples; check your machine's SOCKET/CORE pairs.
From the repository root:

```sh
export LLE_PRODUCER_CPU=0
export LLE_CONSUMER_CPU=2

cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
  -DLLE_BUILD_BENCHMARKS=ON -DLLE_BUILD_APPS=OFF \
  -DLLE_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
  -DLLE_ENABLE_ASAN=OFF -DLLE_ENABLE_TSAN=OFF
cmake --build build/bench --target lle-spsc-benchmark -j4
bash benchmarks/run.sh
```

Build once with the commands above, then use just `bash benchmarks/run.sh` for
subsequent runs. After changing C++ code, rerun the build command yourself: the
script deliberately does not configure, compile, or check whether the binary is stale.
It runs **all 48 cases** (36 throughput and 12 RTT), three repetitions each, with
random interleaving. Defaults favor a quick exploratory comparison, not a final report.
It can also be invoked by its absolute path from another working directory.
The runner needs Bash, `setarch`, `timeout`, `tee`, and `grep` plus standard shell
utilities. Building additionally needs CMake and a C++23 toolchain. Initial
configuration downloads dependencies. Python is no longer required by the runner.

Each invocation creates a unique directory such as
`results/spsc-20260920T120000Z-AbCdEf/` with:

- `benchmarks.json`: both workloads' results, including per-run RTT percentiles;
- `console.log`: benchmark output and diagnostics;
- `complete`: an empty marker created only after a successful run and basic result checks.

Generated result directories are git-ignored and previous results are never
overwritten. A directory without the `complete` marker contains an incomplete or
failed run. The script checks for named results and reported errors, not full JSON
schema validity. CPU selections remain in the benchmark JSON. Extra environment
and build snapshots are no longer collected; record source/compiler/power details
separately for published comparisons.

### ASLR

The runner uses `setarch "$(uname -m)" -R` for the benchmark process. This disables
ASLR for that invocation and its children, not system-wide; no `sudo` or sysctl
change is performed. This reduces one source of address-layout variation but does
not remove scheduling noise. If the host/container denies `setarch -R`, the script
stops with an error rather than silently running with ASLR enabled. Resolve that
host restriction before using this runner. Prefer native Linux for final results
and label WSL measurements separately.

### Select cases or change run settings

After exporting the two CPU variables, select cases for one invocation:

```sh
LLE_BENCH_FILTER='^BM_(LLE|Rigtorp|Boost)<' bash benchmarks/run.sh
LLE_BENCH_FILTER='_RTT<' bash benchmarks/run.sh
LLE_BENCH_FILTER='^BM_(LLE|Rigtorp|Boost)<64>/1024/' bash benchmarks/run.sh
LLE_BENCH_FILTER='^BM_LLE_RTT<64>' bash benchmarks/run.sh
```

These select all throughput cases, all RTT cases, 64-byte/1024-slot throughput
across queues, and only LLE's 64-byte RTT case, respectively. Quote filters because
`<` and `>` have meaning to the shell. If you export a filter globally, use
`unset LLE_BENCH_FILTER` to return to all cases.

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `LLE_PRODUCER_CPU` | required | producer CPU ID |
| `LLE_CONSUMER_CPU` | required | different consumer CPU ID |
| `LLE_BENCH_FILTER` | `.` | Google Benchmark name regex; all cases by default |
| `LLE_BENCH_REPETITIONS` | `3` | repetitions of each selected case |
| `LLE_BENCH_MIN_TIME` | `0.1s` | throughput calibration duration; RTT stays at one fixed batch |
| `LLE_BENCH_TIMEOUT` | `600s` | benchmark timeout |

The old defaults requested roughly 180 seconds of throughput measurements alone
(36 cases × 5 repetitions × 1 second), plus calibration, warm-up, and RTT. The
new defaults request roughly 10.8 seconds of throughput measurements, with the
same extra overheads. These are approximate measurement budgets, not runtime limits.
For a longer comparison, opt in explicitly:

```sh
LLE_BENCH_REPETITIONS=5 LLE_BENCH_MIN_TIME=1s bash benchmarks/run.sh
```

`LLE_BENCH_JOBS` no longer applies because the runner does not build.

For a short smoke test, not a final performance comparison:

```sh
LLE_BENCH_REPETITIONS=1 LLE_BENCH_MIN_TIME=0.01s bash benchmarks/run.sh
```

Your existing `main` still reads the CPU environment variables unchanged. The
runner's other settings use the separate `LLE_BENCH_*` prefix.

## What your benchmark does

The executable compares LLE `SpscRing`, rigtorp `SPSCQueue`, and Boost
`lockfree::spsc_queue`. One producer thread pushes events and one consumer thread
reads them. They share ordinary process memory: this is not a cross-process SHM
or UDP benchmark.

Each throughput iteration transfers **10,000 untimed warm-up events**, followed
by **1,000,000 measured events**. Payload sizes are
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
- `main` reads CPU environment variables, pins the producer, and runs Google Benchmark.

For throughput, queue allocation is outside the benchmark loop. Timing is paused for consumer
creation, the readiness handshake, and warm-up. The producer waits until all
10,000 warm-up events have been consumed and released before resuming timing.
The same queue and pinned threads handle both phases; sequence checks cover both.
Only the million measured events count toward `items_per_second`. No command-line
changes are needed. The timed section includes
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

## Manual build (optional)

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

## Manual runs (optional)

Use the script for automatic result directories. The commands below show the
underlying executable interface, with ASLR disabled for measurements.

First inspect topology and allowed CPUs:

```sh
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
taskset -pc $$
```

Choose two allowed online CPUs on different physical cores (compare SOCKET/CORE
pairs), preferably on the same socket and NUMA node. For example, only if your
topology permits CPU 0 and CPU 2:

```sh
export LLE_PRODUCER_CPU=0
export LLE_CONSUMER_CPU=2
```

The program reads `LLE_PRODUCER_CPU` and `LLE_CONSUMER_CPU` directly from its
environment. Both are required, must contain different nonnegative integer CPU IDs,
and must name CPUs that can be selected. Export them in the same terminal used for
the commands below. Missing, malformed, and out-of-range values fail at startup.
The producer is pinned at startup; consumer affinity is checked when its thread
starts, not when merely listing cases. An unavailable CPU causes an affinity error
instead of silently running unpinned. The old `--producer_cpu` and `--consumer_cpu`
arguments are no longer supported.
Environment parsing precedes Google Benchmark initialization, so both variables
are also required for `--benchmark_list_tests=true` and `--help`.
The main thread is the producer; each consumer pins itself before reporting ready.
Affinity setup is outside measured work. Selected CPUs are included in result context.

List cases first:

```sh
./build/bench/benchmarks/lle-spsc-benchmark \
  --benchmark_list_tests=true
```

Run throughput for all three queues at 64-byte payload and 1024 slots:

```sh
timeout 180s setarch "$(uname -m)" -R ./build/bench/benchmarks/lle-spsc-benchmark \
  --benchmark_filter='^BM_(LLE|Rigtorp|Boost)<64>/1024/' \
  --benchmark_min_time=1s \
  --benchmark_repetitions=3 \
  --benchmark_enable_random_interleaving=true
```

Run only LLE at that size/capacity with `--benchmark_filter='^BM_LLE<64>/1024/'`.
Run all 36 throughput cases and save JSON:

```sh
mkdir -p results
timeout 600s setarch "$(uname -m)" -R ./build/bench/benchmarks/lle-spsc-benchmark \
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

Fixed output filenames in these manual examples overwrite previous results;
the script avoids this by creating unique directories.

Run all 12 RTT cases:

```sh
mkdir -p results
timeout 180s setarch "$(uname -m)" -R ./build/bench/benchmarks/lle-spsc-benchmark \
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

## CLion

For the same ASLR-disabled workflow, run `bash benchmarks/run.sh` in CLion's
terminal after exporting the CPU variables. Running the executable directly from
a normal run configuration does not itself disable ASLR.

Create a Release CMake profile using `../../build/bench` and the `-D` options from the
build command. Reload CMake and select `lle-spsc-benchmark` as the run target.
Put `--benchmark_filter=^BM_LLE<64>/1024/ --benchmark_min_time=1s` in **Program
arguments**, not CMake options. In the run configuration's **Environment variables**,
set `LLE_PRODUCER_CPU=0` and `LLE_CONSUMER_CPU=2`, replacing the example IDs with
your selected CPUs. Do not put CPU options in Program arguments. A terminal's
exports do not necessarily reach an already-running CLion instance.
Run without the debugger. For relative JSON paths, set the working directory to
the repository root and create `results` first.
For RTT, use `--benchmark_filter=^BM_LLE_RTT< --benchmark_repetitions=5` instead.

## Limits of the comparison

- Both threads are pinned, but cores are not isolated. Interrupts, preemption,
  competing workloads, and WSL host scheduling can still affect tails.
- Validation checks sequence order, not every payload byte or metadata field.
  This benchmark does not replace correctness tests.
- The 10,000-transfer warm-up does not touch every slot in a 16384-slot queue
  or guarantee thermal steady state.
- LLE's adapter treats failures as retryable, so a permanent error could stall;
  the runner timeout bounds that situation.
- Queue contracts, compiler visibility, and memory footprints differ. Baseline
  records have 24 bytes of metadata plus payload; LLE rounds its 32-byte metadata
  plus payload to 64-byte slot boundaries. Compare whole workloads, not presumed
  identical assembly operations. Join overhead is timed for throughput, not RTT.

## Future system benchmark methodology

End-to-end system benchmarks should use planned/open-loop arrivals, warm-up,
preallocated and pre-touched sample storage, no disk or console I/O in timed loops,
and environment manifests. Aggregate percentiles should come from pooled raw
observations while preserving run-to-run variation. The current RTT JSON contains
per-run percentiles, not pooled observations.

Published p99.9 requires at least 100,000 observations; p99.99 requires at least
1,000,000. Important A/B comparisons use interleaved blocks and change one variable.

### Protocol batching comparison

The one-stream-per-DATA-packet rule removes `stream_id` from every event frame,
but interleaved streams can force smaller packets and more sends. Benchmark both:

- single-stream-per-packet with one packet-level `stream_id`;
- mixed-stream batching with a `stream_id` in each event frame.

Use the same input stream, event sizes, offered load, flush policy, socket
configuration, and machine placement. Record events per packet, encoded bytes per
event, packets per second, send calls per event, throughput, CPU cost, and end-to-end
latency percentiles. This is planned work, not implemented by the SPSC runner.
