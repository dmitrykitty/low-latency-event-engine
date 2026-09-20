#!/usr/bin/env bash
set -euo pipefail

# configuration comes from the environment; run from any working directory.
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
: "${LLE_PRODUCER_CPU:?export LLE_PRODUCER_CPU first}"
: "${LLE_CONSUMER_CPU:?export LLE_CONSUMER_CPU first}"
export LLE_PRODUCER_CPU LLE_CONSUMER_CPU
filter="${LLE_BENCH_FILTER:-.}"
repetitions="${LLE_BENCH_REPETITIONS:-3}"
min_time="${LLE_BENCH_MIN_TIME:-0.1s}"
run_timeout="${LLE_BENCH_TIMEOUT:-600s}"
binary="$repo_dir/build/bench/benchmarks/lle-spsc-benchmark"

if [[ ! -x "$binary" ]]; then
    echo 'build lle-spsc-benchmark first; see benchmarks/micro/README.md' >&2
    exit 1
fi

# use a unique directory so repeated runs never overwrite previous results.
mkdir -p results
result_dir="$(mktemp -d "$repo_dir/results/spsc-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
printf 'results: %s\n' "$result_dir"

# disable aslr for this run only; failures stop the script via pipefail.
timeout "$run_timeout" setarch "$(uname -m)" -R "$binary" \
    --benchmark_filter="$filter" \
    --benchmark_min_time="$min_time" \
    --benchmark_repetitions="$repetitions" \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="$result_dir/benchmarks.json" --benchmark_out_format=json \
    2>&1 | tee "$result_dir/console.log"

# catch empty selections and reported errors even when the executable returns zero.
if ! grep -q '"name"[[:space:]]*:' "$result_dir/benchmarks.json" ||
    grep -q '"error_occurred"[[:space:]]*:[[:space:]]*true' "$result_dir/benchmarks.json"; then
    echo 'no results or a benchmark failed; check console.log' >&2
    exit 1
fi
touch "$result_dir/complete"
printf 'completed: %s\n' "$result_dir"
