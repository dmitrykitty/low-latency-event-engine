#!/usr/bin/env bash
# One recorded run of both SPSC benchmarks (ping-pong latency and throughput).
# It does not change system settings; use tune.sh for that. Typical use:
#   sudo scripts/bench/tune.sh apply
#   scripts/bench/run_clean.sh
#   sudo scripts/bench/tune.sh restore
#
# Modes (LLE_MODE):
#   multi  (default) every case (queue x payload [x slots]) runs in many separate processes
#          with ASLR on and a different LLE_LAYOUT_SEED each, which also moves every queue
#          object and buffer to a random offset within its page; statistics are taken across
#          processes, so results do not depend on one accidental memory layout.
#   single one process per benchmark with ASLR disabled and in-process repetitions.
#          Repeatable, but every number reflects the single layout that binary happened to get.
#
# Settings come from the environment (defaults in brackets):
#   LLE_PRODUCER_CPU [2]  LLE_CONSUMER_CPU [4]  LLE_BUILD_DIR [build/bench]
#   LLE_BIN_DIR [build dir's benchmarks/]  use prebuilt binaries instead of building
#   LLE_QUEUES ["lle rigtorp boost"]  LLE_SIZES ["16 64 256 1024"]  LLE_SLOTS ["64 1024 16384"]
#   multi:  LLE_PROCESSES [10]  LLE_THROUGHPUT_PROCESSES [5]
#           LLE_LATENCY_SAMPLES [2000000]  LLE_LATENCY_WARMUP [200000]
#   single: LLE_LATENCY_REPS [3]  LLE_THROUGHPUT_REPS [5]
#           LLE_LATENCY_SAMPLES [10000000]  LLE_LATENCY_WARMUP [1000000]
#   both:   LLE_THROUGHPUT_MIN_TIME [1s]
#           LLE_LAYOUT_SEED_BASE [random]  multi: process seeds derive from it
#           LLE_LAYOUT_SEED [0]  single: one seed for the whole run (0 = no page offsets)
# Output: results/clean-<UTC time>/report.md plus raw data; "complete" marks success.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"

mode="${LLE_MODE:-multi}"
if [[ $mode != multi && $mode != single ]]; then
    echo "LLE_MODE must be multi or single" >&2
    exit 2
fi
producer="${LLE_PRODUCER_CPU:-2}"
consumer="${LLE_CONSUMER_CPU:-4}"
build_dir="${LLE_BUILD_DIR:-build/bench}"
bin_dir="${LLE_BIN_DIR:-}"
read -ra queues <<< "${LLE_QUEUES:-lle rigtorp boost}"
read -ra sizes <<< "${LLE_SIZES:-16 64 256 1024}"
read -ra slots_list <<< "${LLE_SLOTS:-64 1024 16384}"
processes="${LLE_PROCESSES:-10}"
throughput_processes="${LLE_THROUGHPUT_PROCESSES:-5}"
latency_reps="${LLE_LATENCY_REPS:-3}"
throughput_reps="${LLE_THROUGHPUT_REPS:-5}"
if [[ $mode == multi ]]; then
    latency_samples="${LLE_LATENCY_SAMPLES:-2000000}"
    latency_warmup="${LLE_LATENCY_WARMUP:-200000}"
else
    latency_samples="${LLE_LATENCY_SAMPLES:-10000000}"
    latency_warmup="${LLE_LATENCY_WARMUP:-1000000}"
fi
throughput_min_time="${LLE_THROUGHPUT_MIN_TIME:-1s}"
if [[ $mode == multi ]]; then
    seed_base="${LLE_LAYOUT_SEED_BASE:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}"
else
    single_seed="${LLE_LAYOUT_SEED:-0}"
fi
cpu_root="/sys/devices/system/cpu"

out="results/clean-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$out"
echo "results: $out ($mode mode)"

# --- build -------------------------------------------------------------------
if [[ -z $bin_dir ]]; then
    if [[ ! -f $build_dir/CMakeCache.txt ]]; then
        echo "configuring $build_dir"
        cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DLLE_BUILD_BENCHMARKS=ON \
            -DLLE_BUILD_APPS=OFF -DLLE_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
            -DLLE_ENABLE_ASAN=OFF -DLLE_ENABLE_TSAN=OFF > "$out/configure.log" 2>&1
    fi
    echo "building benchmarks (log: $out/build.log)"
    cmake --build "$build_dir" --target lle-spsc-latency lle-spsc-benchmark -j"$(nproc)" \
        > "$out/build.log" 2>&1
    bin_dir="$build_dir/benchmarks"
fi
latency_bin="$bin_dir/lle-spsc-latency"
throughput_bin="$bin_dir/lle-spsc-benchmark"
for bin in "$latency_bin" "$throughput_bin"; do
    if [[ ! -x $bin ]]; then
        echo "missing benchmark binary: $bin" >&2
        exit 1
    fi
done

# --- environment checks ------------------------------------------------------
checks=()
warnings=0
check() { # name, recommended, actual, ok(0/1)
    local mark="ok"
    if [[ $4 != 1 ]]; then
        mark="**NOT APPLIED**"
        warnings=$((warnings + 1))
    fi
    checks+=("| $1 | $2 | $3 | $mark |")
}
cpu_in_list() { # cpu, kernel cpu list like "2,4" or "2-5"
    local cpu=$1 part
    IFS=',' read -ra parts <<< "$2"
    for part in "${parts[@]}"; do
        if [[ $part == *-* ]]; then
            ((cpu >= ${part%-*} && cpu <= ${part#*-})) && return 0
        elif [[ -n $part && $part == "$cpu" ]]; then
            return 0
        fi
    done
    return 1
}
read_or() { cat "$1" 2>/dev/null || echo "$2"; }
# per-CPU sysfs values of online CPUs only; offline SMT siblings fail reads with EBUSY.
online_values() { # path below cpuN/, e.g. cpufreq/scaling_governor
    local dir
    for dir in "$cpu_root"/cpu[0-9]*; do
        if [[ ! -e $dir/online || $(cat "$dir/online") == 1 ]]; then
            cat "$dir/$1" 2>/dev/null || true
        fi
    done
}
both_in() { cpu_in_list "$producer" "$1" && cpu_in_list "$consumer" "$1"; }

governors="$(online_values cpufreq/scaling_governor | sort -u | xargs)"
check "cpufreq governor" "performance" "$governors" "$([[ $governors == performance ]] && echo 1 || echo 0)"
no_turbo="$(read_or "$cpu_root/intel_pstate/no_turbo" n/a)"
check "turbo" "off (no_turbo=1)" "no_turbo=$no_turbo" "$([[ $no_turbo == 1 ]] && echo 1 || echo 0)"
smt="$(read_or "$cpu_root/smt/control" n/a)"
check "SMT" "off" "$smt" "$([[ $smt == off || $smt == forceoff ]] && echo 1 || echo 0)"
isolated="$(read_or "$cpu_root/isolated" "")"
check "isolated CPUs (isolcpus)" "includes $producer,$consumer" "${isolated:-none}" \
    "$(both_in "$isolated" && echo 1 || echo 0)"
nohz="$(read_or "$cpu_root/nohz_full" "")"
check "tickless CPUs (nohz_full)" "includes $producer,$consumer" "${nohz:-none}" \
    "$(both_in "$nohz" && echo 1 || echo 0)"
check "benchmark CPUs avoid CPU 0" "yes" "$producer,$consumer" \
    "$([[ $producer != 0 && $consumer != 0 ]] && echo 1 || echo 0)"
core_of() { echo "$(read_or "$cpu_root/cpu$1/topology/physical_package_id" ?)/$(read_or "$cpu_root/cpu$1/topology/core_id" ?)"; }
check "separate physical cores" "yes" "cpu$producer=$(core_of "$producer"), cpu$consumer=$(core_of "$consumer")" \
    "$([[ $(core_of "$producer") != "$(core_of "$consumer")" ]] && echo 1 || echo 0)"
aslr="$(read_or /proc/sys/kernel/randomize_va_space n/a)"
if [[ $mode == multi ]]; then
    check "address-space layout" "randomized per process (randomize_va_space=2)" \
        "randomize_va_space=$aslr" "$([[ $aslr == 2 ]] && echo 1 || echo 0)"
    check "offsets within a page" "randomized per process (LLE_LAYOUT_SEED)" \
        "seeds from base $seed_base" 1
else
    check "address-space layout" "fixed per run (setarch -R)" "fixed per run" 1
    check "offsets within a page" "fixed (single mode)" "LLE_LAYOUT_SEED=$single_seed" 1
fi
ac="$(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -1 || true)"
check "on AC power" "yes" "${ac:-unknown}" "$([[ $ac == 1 ]] && echo 1 || echo 0)"
load1="$(cut -d' ' -f1 /proc/loadavg)"
check "1-minute load average" "< 1.0" "$load1" "$(awk -v l="$load1" 'BEGIN { print (l < 1.0) ? 1 : 0 }')"
busy="$(ps -eo pcpu=,comm= --sort=-pcpu | awk '$1 >= 5 && $2 != "ps" && $2 != "bash" {printf "%s %s%%, ", $2, $1}' | sed 's/, $//')"
check "other busy processes (>=5% CPU)" "none" "${busy:-none}" "$([[ -z $busy ]] && echo 1 || echo 0)"
dirty="$(git status --porcelain --untracked-files=no)"
check "git tree clean" "yes" "$([[ -z $dirty ]] && echo clean || echo "uncommitted changes (saved to code.diff)")" \
    "$([[ -z $dirty ]] && echo 1 || echo 0)"

# --- manifest ------------------------------------------------------------------
compiler="$(grep -m1 '^CMAKE_CXX_COMPILER:' "$build_dir/CMakeCache.txt" | cut -d= -f2)"
{
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $(hostname)"
    echo "mode: $mode"
    echo "commit: $(git rev-parse HEAD) ($(git describe --always --dirty))"
    echo "binaries: $bin_dir"
    echo "latency_sha256: $(sha256sum "$latency_bin" | cut -d' ' -f1)"
    echo "throughput_sha256: $(sha256sum "$throughput_bin" | cut -d' ' -f1)"
    echo "kernel: $(uname -srvmo)"
    echo "cmdline: $(cat /proc/cmdline)"
    echo "compiler: $("$compiler" --version | head -1)"
    echo "build_type: $(grep -m1 '^CMAKE_BUILD_TYPE:' "$build_dir/CMakeCache.txt" | cut -d= -f2)"
    echo "cxx_flags_release: $(grep -m1 '^CMAKE_CXX_FLAGS_RELEASE:' "$build_dir/CMakeCache.txt" | cut -d= -f2)"
    echo "cpus: producer=$producer consumer=$consumer"
    echo "queues: ${queues[*]}  sizes: ${sizes[*]}  slots: ${slots_list[*]}"
    if [[ $mode == multi ]]; then
        echo "latency: processes=$processes samples=$latency_samples warmup=$latency_warmup"
        echo "throughput: processes=$throughput_processes min_time=$throughput_min_time"
    else
        echo "latency: reps=$latency_reps samples=$latency_samples warmup=$latency_warmup"
        echo "throughput: reps=$throughput_reps min_time=$throughput_min_time"
    fi
    echo "randomize_va_space: $aslr"
    if [[ $mode == multi ]]; then
        echo "layout_seed: base=$seed_base; process p, case c uses base + 1000*p + c + 1"
    else
        echo "layout_seed: $single_seed"
    fi
    echo "online_cpus: $(cat "$cpu_root/online")"
    echo "epp: $(online_values cpufreq/energy_performance_preference | sort -u | xargs)"
    echo "loadavg_before: $(cat /proc/loadavg)"
    echo
    echo "## lscpu"
    lscpu
    echo
    echo "## lscpu -e"
    lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE,MAXMHZ,MINMHZ
    echo
    echo "## top processes before run"
    ps -eo pcpu,pmem,comm --sort=-pcpu | head -15
} > "$out/manifest.txt"
if [[ -n $dirty ]]; then
    git diff HEAD > "$out/code.diff"
fi

# per-CPU interrupt totals and thermal throttle counters, read before and after.
irq_total() { # cpu
    awk -v want="CPU$1" 'NR == 1 { for (i = 1; i <= NF; ++i) if ($i == want) col = i + 1; next }
        col && $col ~ /^[0-9]+$/ { sum += $col } END { print sum + 0 }' /proc/interrupts
}
throttle_total() {
    online_values thermal_throttle/core_throttle_count | awk '{ s += $1 } END { print s + 0 }'
}
pkg_temp() {
    local zone
    for zone in /sys/class/thermal/thermal_zone*; do
        if [[ $(cat "$zone/type" 2>/dev/null) == x86_pkg_temp ]]; then
            awk '{ printf "%.1f", $1 / 1000 }' "$zone/temp"
            return
        fi
    done
    echo n/a
}
snapshot() { # label
    echo "$1 irq_cpu$producer=$(irq_total "$producer") irq_cpu$consumer=$(irq_total "$consumer") throttle=$(throttle_total) pkg_temp_c=$(pkg_temp)"
}

# run one benchmark process, recording its context switches; single mode fixes the layout.
timed() { # label, command...
    local label=$1
    shift
    if [[ $mode == single ]]; then
        set -- setarch "$(uname -m)" -R "$@"
    fi
    if [[ -x /usr/bin/time ]]; then
        /usr/bin/time -f "$label voluntary_ctx=%w involuntary_ctx=%c elapsed_s=%e" \
            -a -o "$out/processes.txt" "$@"
    else
        "$@"
    fi
}
ctx_summary() { # phase prefix
    awk -v p="$1" 'index($1, p) == 1 { n++; for (i = 2; i <= NF; ++i) { split($i, kv, "=");
        if (kv[1] != "elapsed_s") s[kv[1]] += kv[2]; else t += kv[2] } }
        END { printf "%s processes=%d voluntary_ctx=%d involuntary_ctx=%d elapsed_s=%.1f\n",
              p, n, s["voluntary_ctx"], s["involuntary_ctx"], t }' "$out/processes.txt"
}
gbench_name() { # queue -> Google Benchmark name
    case "$1" in
    lle) echo LLE ;;
    rigtorp) echo Rigtorp ;;
    boost) echo Boost ;;
    *) echo "unknown queue $1" >&2; return 1 ;;
    esac
}

# --- run -----------------------------------------------------------------------
echo "environment warnings: $warnings (see $out/report.md)"
export LLE_PRODUCER_CPU="$producer" LLE_CONSUMER_CPU="$consumer"
: > "$out/processes.txt"

snapshot before_latency >> "$out/counters.txt"
if [[ $mode == multi ]]; then
    mkdir -p "$out/latency"
    cases=()
    for size in "${sizes[@]}"; do
        for queue in "${queues[@]}"; do
            cases+=("$queue $size")
        done
    done
    echo "running latency: ${#cases[@]} cases x $processes processes"
    for ((p = 0; p < processes; ++p)); do
        echo "  latency round $((p + 1))/$processes"
        # rotate the case order every round so no case always runs first or last.
        for ((c = 0; c < ${#cases[@]}; ++c)); do
            read -r queue size <<< "${cases[$(((c + p * 7) % ${#cases[@]}))]}"
            name="p$(printf '%02d' "$p")-$queue-$size"
            export LLE_LAYOUT_SEED=$((seed_base + 1000 * p + c + 1))
            timed "latency-$name" "$latency_bin" --queues="$queue" --sizes="$size" --reps=1 \
                --samples="$latency_samples" --warmup="$latency_warmup" \
                --csv="$out/latency/$name.csv" > "$out/latency/$name.md" 2> "$out/latency/$name.log"
        done
    done
else
    export LLE_LAYOUT_SEED="$single_seed"
    echo "running latency benchmark"
    timed latency "$latency_bin" --queues="$(IFS=,; echo "${queues[*]}")" \
        --sizes="$(IFS=,; echo "${sizes[*]}")" --reps="$latency_reps" \
        --samples="$latency_samples" --warmup="$latency_warmup" \
        --csv="$out/latency.csv" > "$out/latency.md" 2> "$out/latency.log"
fi
snapshot after_latency >> "$out/counters.txt"

snapshot before_throughput >> "$out/counters.txt"
if [[ $mode == multi ]]; then
    mkdir -p "$out/throughput"
    cases=()
    for size in "${sizes[@]}"; do
        for slots in "${slots_list[@]}"; do
            for queue in "${queues[@]}"; do
                cases+=("$queue $size $slots")
            done
        done
    done
    echo "running throughput: ${#cases[@]} cases x $throughput_processes processes"
    for ((p = 0; p < throughput_processes; ++p)); do
        echo "  throughput round $((p + 1))/$throughput_processes"
        for ((c = 0; c < ${#cases[@]}; ++c)); do
            read -r queue size slots <<< "${cases[$(((c + p * 7) % ${#cases[@]}))]}"
            name="p$(printf '%02d' "$p")-$queue-$size-$slots"
            export LLE_LAYOUT_SEED=$((seed_base + 1000 * p + c + 1))
            timed "throughput-$name" "$throughput_bin" \
                --benchmark_filter="^BM_$(gbench_name "$queue")<$size>/$slots/" \
                --benchmark_min_time="$throughput_min_time" --benchmark_repetitions=1 \
                --benchmark_out="$out/throughput/$name.json" --benchmark_out_format=json \
                > "$out/throughput/$name.log" 2>&1
        done
    done
    errors_in=("$out"/throughput/*.json)
else
    echo "running throughput benchmark"
    filter="^BM_($(for q in "${queues[@]}"; do gbench_name "$q"; done | paste -sd'|'))<($(IFS='|'; echo "${sizes[*]}"))>/($(IFS='|'; echo "${slots_list[*]}"))/"
    timed throughput "$throughput_bin" \
        --benchmark_filter="$filter" \
        --benchmark_min_time="$throughput_min_time" \
        --benchmark_repetitions="$throughput_reps" \
        --benchmark_enable_random_interleaving=true \
        --benchmark_out="$out/throughput.json" --benchmark_out_format=json \
        > "$out/throughput.log" 2>&1
    errors_in=("$out/throughput.json")
fi
snapshot after_throughput >> "$out/counters.txt"
if grep -l '"error_occurred"[[:space:]]*:[[:space:]]*true' "${errors_in[@]}" > /dev/null; then
    echo "a throughput case reported an error; see the logs in $out" >&2
    exit 1
fi
{
    ctx_summary latency
    ctx_summary throughput
} >> "$out/counters.txt"
echo "loadavg_after: $(cat /proc/loadavg)" >> "$out/manifest.txt"

# --- report --------------------------------------------------------------------
{
    echo "# Clean benchmark run $(basename "$out")"
    echo
    echo "Mode \`$mode\`, commit \`$(git describe --always --dirty)\`, binaries \`$bin_dir\`," \
        "CPUs $producer/$consumer, $(grep -m1 '^compiler:' "$out/manifest.txt" | cut -d' ' -f2-)."
    echo
    if [[ $mode == multi ]]; then
        echo "Each latency case ran in $processes separate processes ($latency_samples samples after" \
            "$latency_warmup warm-up each) and each throughput case in $throughput_processes processes," \
            "with ASLR on and every queue object and buffer at a seeded random offset within its" \
            "page (base seed $seed_base). A *run* below is one process."
    else
        echo "One process per benchmark with ASLR disabled; a *run* below is one in-process repetition."
    fi
    echo
    echo "## Environment checklist ($warnings not applied)"
    echo
    echo "| Setting | Recommended | Actual | Status |"
    echo "|---|---|---|---|"
    printf '%s\n' "${checks[@]}"
    echo
    echo "## Disturbance counters"
    echo
    echo '```text'
    cat "$out/counters.txt"
    echo '```'
    echo
} > "$out/report.md"
if command -v python3 > /dev/null; then
    python3 -I - "$out" >> "$out/report.md" <<'PY'
import csv, glob, json, os, re, statistics, sys
out = sys.argv[1]
order = {"lle": 0, "rigtorp": 1, "boost": 2}

# tidy per-run tables, kept for charts.
lat_runs = []
lat_dir = os.path.join(out, "latency")
def offsets(log_path):
    # placements in creation order: object then buffer, request queue before response queue.
    found = []
    if os.path.exists(log_path):
        for line in open(log_path):
            m = re.match(r"layout (object|buffer) bytes=\d+ page_offset=(\d+)", line)
            if m:
                found.append(int(m.group(2)))
    return found

if os.path.isdir(lat_dir):
    for path in sorted(glob.glob(os.path.join(lat_dir, "*.csv"))):
        run = os.path.basename(path).split("-")[0]
        offs = offsets(path[:-4] + ".log") + [""] * 4
        for r in csv.DictReader(open(path)):
            lat_runs.append(dict(r, run=run, req_obj_off=offs[0], req_buf_off=offs[1],
                                 resp_obj_off=offs[2], resp_buf_off=offs[3]))
else:
    for r in csv.DictReader(open(os.path.join(out, "latency.csv"))):
        lat_runs.append(dict(r, run=f"rep{r['rep']}"))
thr_runs = []
thr_dir = os.path.join(out, "throughput")
paths = sorted(glob.glob(os.path.join(thr_dir, "*.json"))) if os.path.isdir(thr_dir) \
    else [os.path.join(out, "throughput.json")]
for path in paths:
    run_label = os.path.basename(path).split("-")[0] if os.path.isdir(thr_dir) else None
    doc = json.load(open(path))
    seed = doc.get("context", {}).get("layout_seed", "")
    offs = (offsets(path[:-5] + ".log") if os.path.isdir(thr_dir) else []) + [""] * 2
    for b in doc["benchmarks"]:
        m = re.match(r"BM_(LLE|Rigtorp|Boost)<(\d+)>/(\d+)/", b["run_name"])
        if m and b.get("run_type") == "iteration":
            thr_runs.append({"queue": m.group(1).lower(), "payload_bytes": int(m.group(2)),
                             "slots": int(m.group(3)),
                             "run": run_label or f"rep{b.get('repetition_index', 0)}",
                             "items_per_second": b["items_per_second"], "layout_seed": seed,
                             "obj_off": offs[0], "buf_off": offs[1]})
lat_fields = ["queue", "payload_bytes", "run", "samples", "min_ns", "p50_ns", "p90_ns", "p99_ns",
              "p999_ns", "p9999_ns", "max_ns", "mean_ns", "layout_seed", "req_obj_off",
              "req_buf_off", "resp_obj_off", "resp_buf_off"]
with open(os.path.join(out, "latency_runs.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=lat_fields, extrasaction="ignore")
    w.writeheader()
    w.writerows(lat_runs)
with open(os.path.join(out, "throughput_runs.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["queue", "payload_bytes", "slots", "run", "items_per_second",
                                      "layout_seed", "obj_off", "buf_off"])
    w.writeheader()
    w.writerows(thr_runs)

def cell(values, fmt):
    return f"{fmt(statistics.median(values))} ({fmt(min(values))}–{fmt(max(values))})"

def t_quantile(df):
    # two-sided 95% Student t critical value; small df from a table, otherwise the
    # Cornish-Fisher expansion around the normal quantile (error < 0.01 for df >= 4).
    table = {1: 12.706, 2: 4.303, 3: 3.182}
    if df in table:
        return table[df]
    z = 1.959964
    return (z + (z**3 + z) / (4 * df) + (5 * z**5 + 16 * z**3 + 3 * z) / (96 * df**2)
            + (3 * z**7 + 19 * z**5 + 17 * z**3 - 15 * z) / (384 * df**3))

def mean_ci(values, fmt):
    if len(values) < 2:
        return fmt(values[0]) if values else ""
    half = t_quantile(len(values) - 1) * statistics.stdev(values) / len(values) ** 0.5
    return f"{fmt(statistics.fmean(values))} ± {fmt(half)}"

whole = lambda v: f"{v:.0f}"
groups = {}
for r in lat_runs:
    groups.setdefault((int(r["payload_bytes"]), r["queue"]), []).append(r)
lat_keys = sorted(groups, key=lambda k: (k[0], order.get(k[1], 9)))

ci_keys = [("p50_ns", "p50"), ("p99_ns", "p99"), ("p999_ns", "p99.9")]
print("## Ping-pong round-trip latency (ns): mean ± 95% CI across runs\n")
print("Each run contributes its own percentile; the interval is for the mean over runs (in")
print("multi mode, over random memory layouts), not for individual round trips.\n")
print("| Payload | Queue | Runs | " + " | ".join(k[1] for k in ci_keys) + " |")
print("|---:|---|---:|" + "---|" * len(ci_keys))
for (size, q) in lat_keys:
    sel = groups[(size, q)]
    cells = [mean_ci([float(r[k]) for r in sel], whole) for k, _ in ci_keys]
    print(f"| {size} B | {q} | {len(sel)} | " + " | ".join(cells) + " |")

keys = [("p50_ns", "p50"), ("p90_ns", "p90"), ("p99_ns", "p99"), ("p999_ns", "p99.9"),
        ("p9999_ns", "p99.99"), ("max_ns", "max")]
print("\n## Ping-pong round-trip latency (ns): median (min–max) across runs\n")
print("| Payload | Queue | Runs | " + " | ".join(k[1] for k in keys) + " |")
print("|---:|---|---:|" + "---|" * len(keys))
for (size, q) in lat_keys:
    sel = groups[(size, q)]
    cells = [cell([float(r[k]) for r in sel], whole) for k, _ in keys]
    print(f"| {size} B | {q} | {len(sel)} | " + " | ".join(cells) + " |")

if any(str(r.get("req_obj_off", "")) != "" for r in lat_runs):
    print("\n## Layout sensitivity: mean p50 (ns) by request-queue offset modulo 128\n")
    print("Offsets are page offsets of the request queue's object and buffer in each run.")
    print("Intel's L2 spatial prefetcher pulls 128-byte line pairs, so offset % 128 decides which")
    print("lines share a pair; a gap between the 0 and 64 columns shows layout sensitivity.\n")
    print("| Payload | Queue | object %128 = 0 | object %128 = 64 | buffer %128 = 0 | buffer %128 = 64 |")
    print("|---:|---|---|---|---|---|")
    for (size, q) in lat_keys:
        sel = groups[(size, q)]
        cells = []
        for field in ("req_obj_off", "req_buf_off"):
            for rem in (0, 64):
                v = [float(r["p50_ns"]) for r in sel
                     if str(r.get(field, "")) != "" and int(r[field]) % 128 == rem]
                cells.append(f"{statistics.fmean(v):.0f} [{len(v)}]" if v else "–")
        print(f"| {size} B | {q} | " + " | ".join(cells) + " |")

qs = sorted({r["queue"] for r in thr_runs}, key=lambda q: order.get(q, 9))
tg = {}
for r in thr_runs:
    tg.setdefault((r["payload_bytes"], r["slots"], r["queue"]), []).append(r["items_per_second"] / 1e6)
thr_keys = sorted({(k[0], k[1]) for k in tg})
two = lambda v: f"{v:.2f}"
for title, fn in (("mean ± 95% CI", mean_ci), ("median (min–max)", cell)):
    print(f"\n## Throughput (M events/s): {title} across runs\n")
    print("| Payload | Slots | Runs | " + " | ".join(qs) + " |")
    print("|---:|---:|---:|" + "---|" * len(qs))
    for size, slots in thr_keys:
        runs = max(len(tg.get((size, slots, q), [])) for q in qs)
        cells = [fn(tg[(size, slots, q)], two) if (size, slots, q) in tg else "" for q in qs]
        print(f"| {size} B | {slots} | {runs} | " + " | ".join(cells) + " |")
PY
else
    echo "python3 not found; see the raw files in $out" >> "$out/report.md"
fi
{
    echo
    echo "## Latency benchmark header (first process)"
    echo
    first="$out/latency.md"
    if [[ $mode == multi ]]; then
        first="$(ls "$out"/latency/*.md | head -1)"
    fi
    sed -n '1,/^$/p;/^- /p' "$first" | awk '!seen[$0]++'
} >> "$out/report.md"

touch "$out/complete"
echo "done: $out/report.md ($warnings environment warnings)"
