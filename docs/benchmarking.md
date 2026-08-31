# Benchmark methodology

System benchmarks use planned/open-loop arrivals, warm-up, preallocated and pre-touched sample storage, no disk or console I/O in timed loops, and environment manifests. Aggregate percentiles are calculated from pooled observations while preserving run-to-run variability.

Published p99.9 needs at least 100,000 observations and p99.99 at least 1,000,000. Important A/B comparisons use interleaved blocks and change one variable.

