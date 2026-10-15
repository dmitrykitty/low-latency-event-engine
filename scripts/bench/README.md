# Benchmark orchestration

## Low-noise recorded run

Two scripts produce one recorded run of both SPSC benchmarks (ping-pong latency
and throughput) for LLE, rigtorp, and Boost:

- [tune.sh](tune.sh) changes CPU settings for the run and restores them afterwards.
- [run_clean.sh](run_clean.sh) builds the benchmarks, checks and records the
  environment, runs both benchmarks, and writes a report. It never changes
  system settings.

```sh
sudo scripts/bench/tune.sh apply      # save current settings, then tune
scripts/bench/run_clean.sh            # about 10-20 minutes
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
| `LLE_PRODUCER_CPU` | `2` | producer / initiator CPU |
| `LLE_CONSUMER_CPU` | `4` | consumer / echo CPU |
| `LLE_BUILD_DIR` | `build/bench` | Release benchmark build tree (configured if missing) |
| `LLE_LATENCY_REPS` | `3` | repetitions of every latency case |
| `LLE_LATENCY_SAMPLES` | `10000000` | measured round trips per case |
| `LLE_LATENCY_WARMUP` | `1000000` | warm-up round trips per case |
| `LLE_THROUGHPUT_REPS` | `5` | repetitions of every throughput case |
| `LLE_THROUGHPUT_MIN_TIME` | `1s` | Google Benchmark minimum time per repetition |

### Output

Each run creates `results/clean-<UTC time>/` (git-ignored):

| File | Contents |
| --- | --- |
| `report.md` | environment checklist, disturbance counters, latency and throughput tables |
| `manifest.txt` | commit, kernel and boot parameters, compiler and flags, `lscpu`, top processes |
| `code.diff` | uncommitted changes, only if the tree was dirty |
| `counters.txt` | interrupts on both CPUs, thermal-throttle count, package temperature, and context switches, before and after each benchmark |
| `latency.csv`, `latency.md` | per-repetition latency percentiles |
| `throughput.json` | Google Benchmark raw output |
| `complete` | created only when the whole run succeeded |

The checklist marks every recommended setting that was not applied as
**NOT APPLIED**, so a report always states the conditions it was measured under.
Commit before a run that will be published, so the results map to one commit.
