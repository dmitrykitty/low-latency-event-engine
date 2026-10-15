#!/usr/bin/env bash
# One recorded run of both SPSC benchmarks (ping-pong latency and throughput).
# It does not change system settings; use tune.sh for that. Typical use:
#   sudo scripts/bench/tune.sh apply
#   scripts/bench/run_clean.sh
#   sudo scripts/bench/tune.sh restore
# Settings come from the environment (defaults in brackets):
#   LLE_PRODUCER_CPU [2]  LLE_CONSUMER_CPU [4]  LLE_BUILD_DIR [build/bench]
#   LLE_LATENCY_REPS [3]  LLE_LATENCY_SAMPLES [10000000]  LLE_LATENCY_WARMUP [1000000]
#   LLE_THROUGHPUT_REPS [5]  LLE_THROUGHPUT_MIN_TIME [1s]
# Output: results/clean-<UTC time>/report.md plus raw data; "complete" marks success.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"

producer="${LLE_PRODUCER_CPU:-2}"
consumer="${LLE_CONSUMER_CPU:-4}"
build_dir="${LLE_BUILD_DIR:-build/bench}"
latency_reps="${LLE_LATENCY_REPS:-3}"
latency_samples="${LLE_LATENCY_SAMPLES:-10000000}"
latency_warmup="${LLE_LATENCY_WARMUP:-1000000}"
throughput_reps="${LLE_THROUGHPUT_REPS:-5}"
throughput_min_time="${LLE_THROUGHPUT_MIN_TIME:-1s}"
cpu_root="/sys/devices/system/cpu"

out="results/clean-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$out"
echo "results: $out"

# --- build -------------------------------------------------------------------
if [[ ! -f $build_dir/CMakeCache.txt ]]; then
    echo "configuring $build_dir"
    cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DLLE_BUILD_BENCHMARKS=ON \
        -DLLE_BUILD_APPS=OFF -DLLE_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
        -DLLE_ENABLE_ASAN=OFF -DLLE_ENABLE_TSAN=OFF > "$out/configure.log" 2>&1
fi
echo "building benchmarks (log: $out/build.log)"
cmake --build "$build_dir" --target lle-spsc-latency lle-spsc-benchmark -j"$(nproc)" \
    > "$out/build.log" 2>&1
latency_bin="$build_dir/benchmarks/lle-spsc-latency"
throughput_bin="$build_dir/benchmarks/lle-spsc-benchmark"

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
both_in() { cpu_in_list "$producer" "$1" && cpu_in_list "$consumer" "$1"; }

governors="$(cat "$cpu_root"/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | xargs)"
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
ac="$(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -1 || true)"
check "on AC power" "yes" "${ac:-unknown}" "$([[ $ac == 1 ]] && echo 1 || echo 0)"
load1="$(cut -d' ' -f1 /proc/loadavg)"
check "1-minute load average" "< 1.0" "$load1" "$(awk -v l="$load1" 'BEGIN { print (l < 1.0) ? 1 : 0 }')"
busy="$(ps -eo pcpu=,comm= --sort=-pcpu | awk -v self="$$" '$1 >= 5 && $2 != "ps" && $2 != "bash" {printf "%s %s%%, ", $2, $1}' | sed 's/, $//')"
check "other busy processes (>=5% CPU)" "none" "${busy:-none}" "$([[ -z $busy ]] && echo 1 || echo 0)"
dirty="$(git status --porcelain --untracked-files=no)"
check "git tree clean" "yes" "$([[ -z $dirty ]] && echo clean || echo "uncommitted changes (saved to code.diff)")" \
    "$([[ -z $dirty ]] && echo 1 || echo 0)"

# --- manifest ------------------------------------------------------------------
compiler="$(grep -m1 '^CMAKE_CXX_COMPILER:' "$build_dir/CMakeCache.txt" | cut -d= -f2)"
{
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $(hostname)"
    echo "commit: $(git rev-parse HEAD) ($(git describe --always --dirty))"
    echo "kernel: $(uname -srvmo)"
    echo "cmdline: $(cat /proc/cmdline)"
    echo "compiler: $("$compiler" --version | head -1)"
    echo "build_type: $(grep -m1 '^CMAKE_BUILD_TYPE:' "$build_dir/CMakeCache.txt" | cut -d= -f2)"
    echo "cxx_flags_release: $(grep -m1 '^CMAKE_CXX_FLAGS_RELEASE:' "$build_dir/CMakeCache.txt" | cut -d= -f2)"
    echo "cpus: producer=$producer consumer=$consumer"
    echo "latency: reps=$latency_reps samples=$latency_samples warmup=$latency_warmup"
    echo "throughput: reps=$throughput_reps min_time=$throughput_min_time"
    echo "online_cpus: $(cat "$cpu_root/online")"
    echo "epp: $(cat "$cpu_root"/cpu*/cpufreq/energy_performance_preference 2>/dev/null | sort -u | xargs)"
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
    cat "$cpu_root"/cpu*/thermal_throttle/core_throttle_count 2>/dev/null | awk '{ s += $1 } END { print s + 0 }'
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

run() { # name, command... ; records context switches via /usr/bin/time when available
    local name=$1
    shift
    snapshot "before_$name" >> "$out/counters.txt"
    if [[ -x /usr/bin/time ]]; then
        /usr/bin/time -f "$name voluntary_ctx=%w involuntary_ctx=%c elapsed_s=%e" -a -o "$out/counters.txt" \
            setarch "$(uname -m)" -R "$@"
    else
        setarch "$(uname -m)" -R "$@"
    fi
    snapshot "after_$name" >> "$out/counters.txt"
}

# --- run -----------------------------------------------------------------------
echo "environment warnings: $warnings (see $out/report.md)"
echo "running latency benchmark (about 5-10 minutes)"
export LLE_PRODUCER_CPU="$producer" LLE_CONSUMER_CPU="$consumer"
run latency "$latency_bin" --reps="$latency_reps" --samples="$latency_samples" \
    --warmup="$latency_warmup" --csv="$out/latency.csv" > "$out/latency.md" 2> "$out/latency.log"

echo "running throughput benchmark (about 5 minutes)"
run throughput "$throughput_bin" \
    --benchmark_filter='^BM_(LLE|Rigtorp|Boost)<' \
    --benchmark_min_time="$throughput_min_time" \
    --benchmark_repetitions="$throughput_reps" \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="$out/throughput.json" --benchmark_out_format=json \
    > "$out/throughput.log" 2>&1
if grep -q '"error_occurred"[[:space:]]*:[[:space:]]*true' "$out/throughput.json"; then
    echo "a throughput case reported an error; see $out/throughput.log" >&2
    exit 1
fi
echo "loadavg_after: $(cat /proc/loadavg)" >> "$out/manifest.txt"

# --- report --------------------------------------------------------------------
{
    echo "# Clean benchmark run $(basename "$out")"
    echo
    echo "Commit \`$(git describe --always --dirty)\`, CPUs $producer/$consumer, $(grep -m1 '^compiler:' "$out/manifest.txt" | cut -d' ' -f2-)."
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
import csv, json, os, re, statistics, sys
out = sys.argv[1]
queues = ["lle", "rigtorp", "boost"]

rows = list(csv.DictReader(open(os.path.join(out, "latency.csv"))))
keys = [("min_ns", "min"), ("p50_ns", "p50"), ("p90_ns", "p90"), ("p99_ns", "p99"),
        ("p999_ns", "p99.9"), ("p9999_ns", "p99.99"), ("max_ns", "max")]
print("## Ping-pong round-trip latency (ns), min–max over repetitions\n")
print("| Payload | Queue | " + " | ".join(k[1] for k in keys) + " |")
print("|---:|---|" + "---:|" * len(keys))
for size in sorted({int(r["payload_bytes"]) for r in rows}):
    for q in queues:
        sel = [r for r in rows if int(r["payload_bytes"]) == size and r["queue"] == q]
        if not sel:
            continue
        cells = []
        for k, _ in keys:
            lo, hi = min(float(r[k]) for r in sel), max(float(r[k]) for r in sel)
            cells.append(f"{lo:.0f}" if round(lo) == round(hi) else f"{lo:.0f}–{hi:.0f}")
        print(f"| {size} B | {q} | " + " | ".join(cells) + " |")

data = json.load(open(os.path.join(out, "throughput.json")))
per = {}
for b in data["benchmarks"]:
    m = re.match(r"BM_(LLE|Rigtorp|Boost)<(\d+)>/(\d+)/", b["run_name"])
    if not m:
        continue
    key = (int(m.group(2)), int(m.group(3)), m.group(1).lower())
    if b.get("run_type") == "iteration":
        per.setdefault(key, []).append(b["items_per_second"] / 1e6)
print("\n## Throughput (M events/s), median (min–max) over repetitions\n")
print("| Payload | Slots | " + " | ".join(queues) + " |")
print("|---:|---:|" + "---|" * len(queues))
for size, slots in sorted({(k[0], k[1]) for k in per}):
    cells = []
    for q in queues:
        v = per.get((size, slots, q))
        cells.append(f"{statistics.median(v):.2f} ({min(v):.2f}–{max(v):.2f})" if v else "")
    print(f"| {size} B | {slots} | " + " | ".join(cells) + " |")
PY
else
    echo "python3 not found; see latency.md and throughput.json for results" >> "$out/report.md"
fi
{
    echo
    echo "## Latency benchmark output"
    echo
    cat "$out/latency.md"
} >> "$out/report.md"

touch "$out/complete"
echo "done: $out/report.md ($warnings environment warnings)"
