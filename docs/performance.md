# Performance report

No representative results yet: nothing below has been measured on an isolated,
fixed-frequency machine. The sections below are an **exploratory engineering
record** of SPSC ring optimizations, measured on a developer laptop with other
applications running. Use them for relative, same-session comparisons only.

Representative results will include raw data and environment manifests for rate, payload, fan-out, batching, busy-poll, and reliability experiments. Negative results and reverted optimizations remain part of the engineering record.

## SPSC ring hot path

### Environment and method (all experiments below)

- Intel Core i7-10750H (6 cores / 12 threads), Linux 7.0.0-27-generic, GCC 15.2.0,
  CMake Release (`-O3 -DNDEBUG`).
- `powersave` governor (intel_pstate), turbo on, SMT on, no `isolcpus`; an IDE
  was running in the background. ASLR disabled for the benchmark process.
- Throughput: `lle-spsc-benchmark`, 1024 slots, `--benchmark_min_time=1s`,
  5 repetitions, reported as median (min–max). Its consumer reads only the sequence
  number, so large-payload throughput mostly measures producer-side copying.
- Latency: `lle-spsc-latency` ping-pong round trip, 10M samples after 1M warm-up
  per run, two runs per variant, reported as the min–max of the per-run
  percentile. See the [benchmark guide](../benchmarks/micro/README.md).
- All three queues run inside the same executable, so each variant treats them
  identically. Variants were run interleaved in one session; numbers from
  different sessions are not comparable.
- After every change, all 131 tests pass under ASan+UBSan and under TSan.

### 1. Cached peer cursor and ring geometry — kept

Each `SpscRing` handle keeps a private copy of the other side's cursor and reloads
it only when that copy says the ring is full (producer) or empty (consumer). Slot
count, stride, and payload capacity are copied once when the handle is created
instead of being reread from shared memory on every operation. See
[memory-model.md](memory-model.md#producer-publication) for the ordering argument.

Same session, CPUs 0 and 2, 64-byte payload throughput: LLE 23.80 (22.90–28.58)
→ 57.72 (32.21–76.15) M events/s; rigtorp in the same session 82.77 (71.81–95.81).

### 2. Link-time optimization — measured, not adopted

Hypothesis: `try_publish`, `try_acquire`, and `release` were compiled in
`spsc_ring.cpp` inside the static library, so callers could not inline them, while
rigtorp and Boost are header-only.

With `INTERPROCEDURAL_OPTIMIZATION` on `lle` and the benchmarks, `objdump` showed
the ring calls disappear (24 → 0 in `lle-spsc-latency`; 52 → 8 in
`lle-spsc-benchmark`, the 8 remaining in its older RTT functions). 64-byte
throughput rose sharply, confirming the hypothesis. LTO was not kept as the fix:

- applications that link `liblle.a` without LTO would not get the benefit;
- LTO also changed code generation of the header-only baselines in the same
  executables (rigtorp and Boost throughput moved too), which muddies comparisons.

### 3. Hot path defined inline in the header — kept

`SpscRing::try_publish`, `SpscRing::try_acquire`, `SpscRing::release`, and their
helpers `fill_slot_header` and `invalid_slot` moved unchanged from
`src/shm/spsc_ring.cpp` into [spsc_ring.hpp](../include/shm/spsc_ring.hpp) as
`inline` definitions; a textual diff of the bodies before and after is empty apart
from `inline` and the `detail::` qualifier. Setup and validation (`initialize`,
`attach`, `required_bytes`, `close_publication`, constructors) stay in the `.cpp`.
The public API and SHM layout are unchanged.

```cpp
// include/shm/spsc_ring.hpp (after the class definition)
namespace lle::detail {
inline void fill_slot_header(shm::RingSlotHeader* header, const EventView& event) noexcept;
inline bool invalid_slot(const shm::RingSlotHeader& slot, std::uint32_t payload_capacity) noexcept;
}
namespace lle::shm {
inline PublishResult SpscRing::try_publish(const EventView& event) noexcept { /* ... */ }
inline std::expected<EventView, AcquireError> SpscRing::try_acquire() noexcept { /* ... */ }
inline bool SpscRing::release() noexcept { /* ... */ }
}
```

`objdump` confirms zero out-of-line `SpscRing` hot-path calls in both benchmark
executables without any LTO flag.

Three variants, interleaved in one session (CPUs 2 and 4): ring in `.cpp`
(previous state), ring in `.cpp` with LTO, ring inline in the header.
Raw outputs: `results/ab-hotpath-20261015T201526Z/` (local, git-ignored).

Throughput, 64-byte payload, M events/s:

| Variant | LLE | rigtorp | Boost |
|---|---|---|---|
| `.cpp` | 70.10 (37.97–79.21) | 68.50 (57.48–81.67) | 17.10 (16.88–17.33) |
| `.cpp` + LTO | 105.44 (98.68–124.29) | 72.51 (68.85–81.64) | 21.57 (21.39–21.66) |
| header | 112.41 (101.80–122.89) | 71.39 (63.36–78.34) | 16.46 (16.38–16.98) |

At 1024 bytes LLE measured 12.86 / 13.56 / 13.14 M events/s (`.cpp` / LTO / header)
against rigtorp 8.56–8.75 and Boost 7.92–8.02 across the three variants; see the
consumer caveat above.

Ping-pong round trip, p50 and p99 in ns (min–max over two 10M-sample runs).
rigtorp and Boost are shown from the header variant. Their code is identical in
the `.cpp` and header variants, and their results moved less between those two
variants than LLE's did.

| Payload | LLE `.cpp` p50 / p99 | LLE header p50 / p99 | rigtorp p50 / p99 | Boost p50 / p99 |
|---|---|---|---|---|
| 16 B | 294–313 / 348–368 | 258–264 / 328–353 | 302–306 / 371–404 | 266–278 / 347–357 |
| 64 B | 309–323 / 381–406 | 280–292 / 340–349 | 322–329 / 401–425 | 263–277 / 353–380 |
| 256 B | 434–451 / 543–568 | 350–369 / 421–461 | 432–456 / 516–534 | 396–410 / 486–496 |
| 1024 B | 657–663 / 850–873 | 598–614 / 730–735 | 623–647 / 734–769 | 610–620 / 710–735 |

Observations:

- Header-inline matches or beats LTO on 64-byte throughput without any build flag,
  and leaves the baselines' code generation alone.
- Round-trip latency improved at every payload size. In this session LLE has the
  lowest p50 at 16 and 256 bytes, overlaps Boost at 1024 bytes, and Boost has the
  lowest p50 at 64 bytes.
- The latency gain is larger than the cost of a function call alone, and larger
  than the LTO variant's. Why is not established; candidate explanations
  (register allocation across the spin loop, how `std::expected` is returned,
  code alignment) need `perf` data before they can be claimed.
- Ranges are wide and the machine was not isolated. Representative numbers must
  come from a quiet, fixed-frequency, isolated-core run.
