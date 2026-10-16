# Result analysis

[plot_results.py](plot_results.py) renders the charts in
[docs/performance.md](../../docs/performance.md) from a `run_clean.sh` result
directory, using only the Python standard library:

```sh
python3 scripts/analysis/plot_results.py results/representative/2026-10-16-spsc figures/performance
```

It reads `latency_runs.csv` and `throughput_runs.csv` and writes `latency-p50`,
`latency-p99`, and `throughput-1024` charts as static SVG, each in a light and a
dark variant (`<name>-light.svg`, `<name>-dark.svg`). Latency charts show every
process as a dot plus the mean and its 95% confidence interval; the throughput
chart shows the mean and interval per queue and payload size.
