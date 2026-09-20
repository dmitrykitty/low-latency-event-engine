# Benchmark methodology

The current Google Benchmark suite compares LLE, Boost, and rigtorp SPSC queues
using throughput and closed-loop ping-pong RTT. These are thread-to-thread
microbenchmarks, not end-to-end IPC or network measurements.

See the [benchmark README](../benchmarks/micro/README.md) for the runner script,
CPU affinity and ASLR setup, all/selected-case commands, saved results,
interpretation, limitations, and planned system benchmark methodology.
