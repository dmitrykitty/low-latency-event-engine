# System benchmarks

`lle-e2e-benchmark` measures the complete engine path on one host:

```text
producer -> ingress ring (SHM) -> lle::Sender -> UDP loopback -> lle::Receiver
         -> egress ring (SHM) -> consumer
```

Four threads run in one process, each pinned to its own CPU:
`LLE_E2E_CPUS=producer,sender,receiver,consumer` (default `2,3,4,5`). The engine is
built exactly as an application would build it: the producer creates and initializes
the ingress ring, the Receiver creates the egress ring, the Sender and the consumer
attach.

## Modes

- **latency** — closed loop, one event in flight. The producer stamps a fenced `rdtsc`
  into `source_timestamp_ns` immediately before `try_publish`; the consumer reads
  `rdtsc` immediately after `try_acquire`. The result is **one-way** latency measured
  across two cores, so the program first estimates the TSC offset between the producer
  and consumer CPUs (fastest of 100,000 request/reply exchanges) and prints it with its
  error bound. An event that does not arrive within `--timeout-ms` is counted as lost.
- **throughput** — the producer publishes as fast as the ingress ring accepts. The
  report separates what reached the consumer, what the Receiver's gap detector counted
  as missing, what was dropped at a full egress ring, and the kernel's system-wide UDP
  `RcvbufErrors` during the case.

The consumer copies every payload and checks the sequence number in both the event
metadata and the payload; the latency run also requires zero loss to be meaningful
(see the engine counter table it prints).

## Build and run

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DLLE_BUILD_BENCHMARKS=ON
cmake --build build/bench --target lle-e2e-benchmark
LLE_E2E_CPUS=2,3,4,5 build/bench/benchmarks/lle-e2e-benchmark --latency-csv=latency.csv --throughput-csv=throughput.csv
```

Options: `--modes=latency,throughput`, `--samples`, `--warmup`, `--events`,
`--sizes=16,64,256,1024`, `--egress-slots`, `--timeout-ms`, `--allow-smt-siblings`.

For representative numbers use the same machine preparation as the SPSC benchmarks
([scripts/bench](../../scripts/bench/README.md)), with all four CPUs isolated, for
example `isolcpus=2,3,4,5 nohz_full=2,3,4,5 rcu_nocbs=2,3,4,5 irqaffinity=0,1`.

## What this does and does not measure

- UDP goes through the kernel's loopback device: the numbers include the system calls,
  the network stack, and waking the Receiver thread from `poll()`, but no NIC, cable,
  or switch.
- The Receiver sleeps in `poll()` between packets, so its CPU may enter an idle state;
  the program prints the receiver CPU's idle states and their exit latencies.
- One-way latency includes one fenced `rdtsc` pair and the cross-core TSC offset printed
  in the header.
- Closed loop: the latency of an idle engine, not under sustained or bursty load.
