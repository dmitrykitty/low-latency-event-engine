# System benchmarks

Status: planned. This directory does not yet contain the open-loop producer,
consumer, multi-host orchestration, or end-to-end latency measurement programs.

The implemented measurement suite covers thread-to-thread SPSC throughput and
closed-loop RTT. Use the [microbenchmark guide](../micro/README.md) and
[runner](../run.sh) for those experiments.

The complete SHM -> UDP -> SHM path currently has a correctness test in
[receiver_test.cpp](../../tests/integration/receiver_test.cpp). That test does not
produce an end-to-end performance dataset. See the
[benchmark methodology](../../docs/benchmarking.md) for the current measurement
scope and the planned system methodology.

