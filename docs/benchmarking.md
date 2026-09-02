# Benchmark methodology

System benchmarks use planned/open-loop arrivals, warm-up, preallocated and pre-touched sample storage, no disk or console I/O in timed loops, and environment manifests. Aggregate percentiles are calculated from pooled observations while preserving run-to-run variability.

Published p99.9 needs at least 100,000 observations and p99.99 at least 1,000,000. Important A/B comparisons use interleaved blocks and change one variable.

## Protocol batching comparison

The one-stream-per-DATA-packet rule removes `stream_id` from every event frame, but interleaved streams can force smaller packets and more sends. Benchmark both layouts before treating the current choice as final:

- single-stream-per-packet with one packet-level `stream_id`;
- mixed-stream batching with a `stream_id` in each event frame.

Use the same input event stream, event sizes, offered load, flush policy, socket configuration, and machine placement for both variants. Record events per packet, encoded bytes per event, packets per second, send calls per event, throughput, CPU cost, and end-to-end latency percentiles.
