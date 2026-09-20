#!/usr/bin/env bash
set -euo pipefail

# configuration comes from the environment; run from any working directory.
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
: "${LLE_PRODUCER_CPU:?export LLE_PRODUCER_CPU first}"
: "${LLE_CONSUMER_CPU:?export LLE_CONSUMER_CPU first}"
export LLE_PRODUCER_CPU LLE_CONSUMER_CPU
filter="${LLE_BENCH_FILTER:-.}"
repetitions="${LLE_BENCH_REPETITIONS:-5}"
min_time="${LLE_BENCH_MIN_TIME:-1s}"
run_timeout="${LLE_BENCH_TIMEOUT:-600s}"
jobs="${LLE_BENCH_JOBS:-4}"

for command in cmake setarch timeout tee python3; do
    command -v "$command" >/dev/null || { printf 'missing command: %s\n' "$command" >&2; exit 1; }
done
for value in "$repetitions" "$jobs"; do
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || { printf 'repetitions and jobs must be positive integers\n' >&2; exit 1; }
done

# do not silently fall back to an aslr-enabled run.
architecture="$(uname -m)"
if ! setarch "$architecture" -R true; then
    printf 'setarch -R is not permitted on this host; no benchmark was run.\n' >&2
    exit 1
fi

cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release \
    -DLLE_BUILD_BENCHMARKS=ON -DLLE_BUILD_APPS=OFF \
    -DLLE_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
    -DLLE_ENABLE_ASAN=OFF -DLLE_ENABLE_TSAN=OFF
cmake --build build/bench --target lle-spsc-benchmark -j "$jobs"

# use a unique directory so repeated runs never overwrite previous results.
mkdir -p results
result_dir="$(mktemp -d "$repo_dir/results/spsc-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
printf 'results: %s\n' "$result_dir"
{
    printf 'producer_cpu=%s\nconsumer_cpu=%s\nfilter=%s\nrepetitions=%s\nmin_time=%s\ntimeout=%s\naslr=disabled via setarch -R\n' \
        "$LLE_PRODUCER_CPU" "$LLE_CONSUMER_CPU" "$filter" "$repetitions" "$min_time" "$run_timeout"
    uname -a
    if command -v lscpu >/dev/null; then lscpu; fi
    if command -v git >/dev/null; then git rev-parse HEAD; git status --short; fi
} > "$result_dir/environment.txt"
cp build/bench/CMakeCache.txt "$result_dir/CMakeCache.txt"
printf 'running\n' > "$result_dir/status.txt"

if timeout "$run_timeout" setarch "$architecture" -R \
    ./build/bench/benchmarks/lle-spsc-benchmark \
    --benchmark_filter="$filter" \
    --benchmark_min_time="$min_time" \
    --benchmark_repetitions="$repetitions" \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="$result_dir/benchmarks.json" --benchmark_out_format=json \
    2>&1 | tee "$result_dir/console.log"; then
    # google benchmark can report a failed case while returning exit code zero.
    if python3 - "$result_dir/benchmarks.json" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as source:
        cases = json.load(source).get("benchmarks", [])
except (OSError, ValueError) as error:
    sys.exit(f"missing or invalid result json (check console.log): {error}")
if not cases or any(case.get("error_occurred", False) for case in cases):
    sys.exit("no matching benchmarks or at least one benchmark reported an error")
PY
    then
        printf 'complete\n' > "$result_dir/status.txt"
        printf 'completed: %s\n' "$result_dir"
    else
        printf 'failed validation\n' > "$result_dir/status.txt"
        exit 1
    fi
else
    result=$?
    printf 'failed: exit %s\n' "$result" > "$result_dir/status.txt"
    printf 'benchmark failed or timed out; see %s/console.log\n' "$result_dir" >&2
    exit "$result"
fi
