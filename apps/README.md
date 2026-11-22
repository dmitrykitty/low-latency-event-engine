# Programs

| Program | Role |
|---|---|
| `lle-sender` | attaches to an ingress ring and sends each event as an LLEP DATA packet to one or more receivers |
| `lle-receiver` | listens on UDP, validates packets, and publishes events into an egress ring it creates |
| `lle-demo-producer` | stand-in application: creates the ingress ring and publishes numbered events at a set rate |
| `lle-demo-consumer` | stand-in application: reads the egress ring, checks order and contents, reports loss and latency |

Each program prints its options with `--help`. All four stop cleanly on SIGINT or SIGTERM
and print their counters. `--cpu=N` pins a program's thread to one CPU.

## One host

```bash
cmake --preset dev && cmake --build --preset dev
scripts/demo/run_local.sh
```

The script starts the four programs in ownership order and shuts them down in order (see
its header comment). Settings: `LLE_DEMO_COUNT`, `LLE_DEMO_RATE`, `LLE_DEMO_PAYLOAD`,
`LLE_DEMO_CPUS=producer,sender,receiver,consumer`, `LLE_DEMO_STRICT=1` to fail on any loss.
CTest runs it as `lle.apps.local_demo`.

## Two hosts

On the receiving host (B):

```bash
build/dev/apps/lle-receiver --listen=0.0.0.0:47000 --egress=/lle-out
build/dev/apps/lle-demo-consumer --egress=/lle-out --latency=off --progress
```

On the sending host (A), with B's address:

```bash
build/dev/apps/lle-demo-producer --ingress=/lle-in --count=1000000 --rate=100000 --start-delay-ms=1000
build/dev/apps/lle-sender --ingress=/lle-in --dest=<B-address>:47000
```

Add more `--dest` options to fan out to several receivers. Stop the receiver with Ctrl+C
when the sender has exited; the consumer then drains the ring and prints its summary.

Latency from `lle-demo-consumer` compares the producer's `CLOCK_MONOTONIC` timestamp with
the consumer's, so it is meaningful only when both run on the same host; use
`--latency=off` across hosts. For measured results use the
[end-to-end benchmark](../benchmarks/system/README.md).

## Startup and shutdown rules

- The creator of a ring starts first: the producer creates ingress, the receiver creates
  egress. `lle-sender` and `lle-demo-consumer` retry attaching for `--wait-ms`.
- The producer closes ingress when it finishes; the sender drains the ring and exits.
- Stopping the receiver closes egress; the consumer reads what is queued and exits.
- A program that crashes leaves its segment in `/dev/shm`; remove it with `rm` before
  reusing the name (creation uses `O_EXCL` and never reuses a stale segment).
