# Benchmark orchestration

## Low-noise recorded run

Two scripts produce one recorded run of both SPSC benchmarks (ping-pong latency
and throughput) for LLE, rigtorp, and Boost:

- [tune.sh](tune.sh) changes CPU settings for the run and restores them afterwards.
- [run_clean.sh](run_clean.sh) builds the benchmarks, checks and records the
  environment, runs both benchmarks, and writes a report. It never changes
  system settings.

### Why many processes with ASLR on (default `multi` mode)

Queue latency on this hardware depends strongly on where the queues, their
handles, and the threads' stacks happen to land in memory: cache-line sharing,
4K address aliasing, and cache-set conflicts all follow from addresses. With ASLR
disabled, every run of a binary gets the same layout, so results are repeatable
but reflect that one accidental layout; any code change shifts it. The default
`multi` mode therefore runs every case (queue × payload, and × slots for
throughput) in many separate processes with ASLR on, in a rotating order, and
reports the median and min–max across processes. ASLR only moves whole pages,
and the effects that matter depend on offsets within a page, so each process
also gets its own `LLE_LAYOUT_SEED`: the benchmarks then place every queue
object and buffer (LLE's ring, rigtorp's and Boost's slot arrays) at a random
64-byte multiple within its page, with 128-byte guards on both sides. The chosen
offsets are logged and copied into `latency_runs.csv` / `throughput_runs.csv`.
`LLE_MODE=single` keeps the older behaviour: one process per benchmark, ASLR
disabled, in-process repetitions.

```sh
sudo scripts/bench/tune.sh apply      # save current settings, then tune
scripts/bench/run_clean.sh            # about 15 minutes with the defaults; no sudo needed
sudo scripts/bench/tune.sh restore    # put the saved settings back
```

`tune.sh apply` sets the cpufreq governor to `performance` (intel_pstate then uses
the `performance` energy preference), disables turbo, and disables SMT. It saves the
previous values in `/run/lle-bench-tune.state`; `restore` puts them back. None of
this survives a reboot. `scripts/bench/tune.sh status` shows the current values
without sudo.

Before running, close other applications (IDEs, browsers) and keep the laptop on
AC power. The run uses CPUs 2 and 4 by default: separate physical cores, away from
CPU 0, which handles the most interrupts.

### Core isolation (optional, needs a reboot)

For the cleanest tails, also keep the scheduler, timer tick, RCU callbacks, and
interrupts off the two benchmark CPUs. Add to `GRUB_CMDLINE_LINUX_DEFAULT` in
`/etc/default/grub`:

```text
isolcpus=2,4 nohz_full=2,4 rcu_nocbs=2,4 irqaffinity=0,1,3,5
```

then run `sudo update-grub` and reboot. Remove the parameters and run
`sudo update-grub` again when you are done. The benchmarks pin their threads
explicitly, so they still run on isolated CPUs.

### Settings

| Variable | Default | Meaning |
| --- | --- | --- |
| `LLE_MODE` | `multi` | `multi` (processes, ASLR on) or `single` (repetitions, ASLR off) |
| `LLE_PRODUCER_CPU` | `2` | producer / initiator CPU |
| `LLE_CONSUMER_CPU` | `4` | consumer / echo CPU |
| `LLE_BUILD_DIR` | `build/bench` | Release benchmark build tree (configured if missing) |
| `LLE_BIN_DIR` | build tree | measure prebuilt binaries instead of building (A/B comparisons) |
| `LLE_QUEUES` | `lle rigtorp boost` | queues to run |
| `LLE_SIZES` | `16 64 256 1024` | payload sizes in bytes |
| `LLE_SLOTS` | `64 1024 16384` | throughput queue capacities |
| `LLE_PROCESSES` | `10` | multi: processes per latency case |
| `LLE_THROUGHPUT_PROCESSES` | `5` | multi: processes per throughput case |
| `LLE_LATENCY_SAMPLES` | `2000000` (multi), `10000000` (single) | measured round trips per process or repetition |
| `LLE_LATENCY_WARMUP` | `200000` (multi), `1000000` (single) | warm-up round trips |
| `LLE_LATENCY_REPS` | `3` | single: repetitions of every latency case |
| `LLE_THROUGHPUT_REPS` | `5` | single: repetitions of every throughput case |
| `LLE_THROUGHPUT_MIN_TIME` | `1s` | Google Benchmark minimum time per run |
| `LLE_LAYOUT_SEED_BASE` | random | multi: process seeds are base + 1000·process + case + 1 |
| `LLE_LAYOUT_SEED` | `0` | single: one layout seed for the run; 0 keeps every offset fixed |

### Output

Each run creates `results/clean-<UTC time>/` (git-ignored):

| File | Contents |
| --- | --- |
| `report.md` | environment checklist, disturbance counters, latency and throughput tables: mean ± 95% CI across runs, median (min–max), and latency by request-queue offset % 128 |
| `latency_runs.csv`, `throughput_runs.csv` | one row per case and run (process or repetition), for charts |
| `manifest.txt` | mode, commit, binary hashes, kernel and boot parameters, compiler and flags, `lscpu`, top processes |
| `code.diff` | uncommitted changes, only if the tree was dirty |
| `counters.txt` | interrupts on both CPUs, thermal-throttle count, and package temperature before and after each benchmark; context-switch totals |
| `processes.txt` | context switches and elapsed time of every benchmark process |
| `latency/`, `throughput/` | multi: raw per-process output |
| `latency.csv`, `latency.md`, `throughput.json` | single: raw output |
| `complete` | created only when the whole run succeeded |

### Reading the tables

A *run* is one process in `multi` mode and one repetition in `single` mode; each
run contributes its own percentiles. The headline tables give the mean over runs
with a 95% confidence interval (Student t), which in `multi` mode is the expected
value over random memory layouts. Layout effects make this distribution bimodal
for some queues, so the report also shows median (min–max) and the mean p50 split
by the request queue's object and buffer offset modulo 128 (Intel's spatial
prefetcher works on 128-byte line pairs). More processes narrow the interval;
more samples per process do not, because a process keeps one layout.

The checklist marks every recommended setting that was not applied as
**NOT APPLIED**, so a report always states the conditions it was measured under.
Commit before a run that will be published, so the results map to one commit.
