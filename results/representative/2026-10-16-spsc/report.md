# Clean benchmark run clean-20261016T164133Z

Mode `multi`, commit `ce4ae8d`, binaries `build/bench/benchmarks`, CPUs 2/4, c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0.

Each latency case ran in 32 separate processes (2000000 samples after 200000 warm-up each) and each throughput case in 10 processes, with ASLR on and every queue object and buffer at a seeded random offset within its page (base seed 257400851). A *run* below is one process.

## Environment checklist (1 not applied)

| Setting | Recommended | Actual | Status |
|---|---|---|---|
| cpufreq governor | performance | performance | ok |
| turbo | off (no_turbo=1) | no_turbo=1 | ok |
| SMT | off | off | ok |
| isolated CPUs (isolcpus) | includes 2,4 | 2,4 | ok |
| tickless CPUs (nohz_full) | includes 2,4 | 2,4 | ok |
| benchmark CPUs avoid CPU 0 | yes | 2,4 | ok |
| separate physical cores | yes | cpu2=0/2, cpu4=0/4 | ok |
| address-space layout | randomized per process (randomize_va_space=2) | randomize_va_space=2 | ok |
| offsets within a page | randomized per process (LLE_LAYOUT_SEED) | seeds from base 257400851 | ok |
| on AC power | yes | 1 | ok |
| 1-minute load average | < 1.0 | 0.73 | ok |
| other busy processes (>=5% CPU) | none | gnome-shell 9.7%, localsearch-ext 6.4% | **NOT APPLIED** |
| git tree clean | yes | clean | ok |

## Disturbance counters

```text
before_latency irq_cpu2=35416 irq_cpu4=31781 throttle=0 pkg_temp_c=42.0
after_latency irq_cpu2=37026 irq_cpu4=66828 throttle=0 pkg_temp_c=46.0
before_throughput irq_cpu2=37026 irq_cpu4=66828 throttle=0 pkg_temp_c=47.0
after_throughput irq_cpu2=115594 irq_cpu4=67639 throttle=0 pkg_temp_c=47.0
latency processes=384 voluntary_ctx=808 involuntary_ctx=1158 elapsed_s=842.4
throughput processes=360 voluntary_ctx=13290 involuntary_ctx=26206 elapsed_s=726.1
```

## Ping-pong round-trip latency (ns): mean ± 95% CI across runs

Each run contributes its own percentile; the interval is for the mean over runs (in
multi mode, over random memory layouts), not for individual round trips.

| Payload | Queue | Runs | p50 | p99 | p99.9 |
|---:|---|---:|---|---|---|
| 16 B | lle | 32 | 304 ± 1 | 355 ± 4 | 386 ± 6 |
| 16 B | rigtorp | 32 | 328 ± 8 | 404 ± 9 | 429 ± 9 |
| 16 B | boost | 32 | 353 ± 2 | 417 ± 3 | 439 ± 4 |
| 64 B | lle | 32 | 366 ± 2 | 424 ± 4 | 451 ± 4 |
| 64 B | rigtorp | 32 | 399 ± 13 | 459 ± 13 | 487 ± 11 |
| 64 B | boost | 32 | 408 ± 3 | 455 ± 3 | 470 ± 3 |
| 256 B | lle | 32 | 527 ± 2 | 624 ± 4 | 646 ± 4 |
| 256 B | rigtorp | 32 | 569 ± 12 | 637 ± 12 | 670 ± 12 |
| 256 B | boost | 32 | 578 ± 2 | 637 ± 2 | 666 ± 3 |
| 1024 B | lle | 32 | 893 ± 2 | 962 ± 3 | 999 ± 5 |
| 1024 B | rigtorp | 32 | 881 ± 13 | 969 ± 14 | 1017 ± 17 |
| 1024 B | boost | 32 | 889 ± 3 | 971 ± 4 | 1012 ± 7 |

## Ping-pong round-trip latency (ns): median (min–max) across runs

| Payload | Queue | Runs | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---:|---|---:|---|---|---|---|---|---|
| 16 B | lle | 32 | 305 (296–310) | 322 (306–334) | 354 (329–387) | 390 (356–420) | 411 (375–2518) | 490 (418–3750) |
| 16 B | rigtorp | 32 | 334 (292–366) | 378 (339–405) | 412 (357–448) | 435 (383–479) | 460 (409–2414) | 530 (488–3627) |
| 16 B | boost | 32 | 353 (343–368) | 401 (390–417) | 418 (404–434) | 437 (422–466) | 449 (440–2475) | 503 (467–3311) |
| 64 B | lle | 32 | 367 (356–375) | 387 (380–410) | 424 (405–444) | 450 (432–490) | 478 (454–504) | 583 (488–1294) |
| 64 B | rigtorp | 32 | 407 (350–461) | 447 (375–494) | 474 (398–519) | 496 (432–536) | 526 (472–2647) | 590 (550–4220) |
| 64 B | boost | 32 | 410 (391–421) | 435 (415–444) | 456 (437–468) | 472 (451–481) | 486 (465–2824) | 531 (498–3873) |
| 256 B | lle | 32 | 527 (520–535) | 581 (574–588) | 630 (596–638) | 647 (617–664) | 669 (639–3283) | 779 (705–4130) |
| 256 B | rigtorp | 32 | 590 (525–624) | 625 (559–661) | 653 (590–685) | 681 (623–729) | 701 (667–3380) | 853 (723–4559) |
| 256 B | boost | 32 | 579 (566–590) | 613 (600–623) | 638 (627–649) | 666 (652–679) | 687 (671–3319) | 747 (704–4537) |
| 1024 B | lle | 32 | 893 (883–903) | 929 (915–940) | 961 (944–978) | 997 (978–1035) | 1025 (1003–3646) | 1284 (1087–4849) |
| 1024 B | rigtorp | 32 | 880 (833–943) | 923 (875–989) | 967 (917–1035) | 1023 (955–1196) | 1059 (996–3755) | 1272 (1079–4714) |
| 1024 B | boost | 32 | 890 (851–905) | 931 (894–946) | 972 (934–999) | 1005 (987–1064) | 1038 (1012–3764) | 1225 (1074–1958116) |

## Layout sensitivity: mean p50 (ns) by request-queue offset modulo 128

Offsets are page offsets of the request queue's object and buffer in each run.
Intel's L2 spatial prefetcher pulls 128-byte line pairs, so offset % 128 decides which
lines share a pair; a gap between the 0 and 64 columns shows layout sensitivity.

| Payload | Queue | object %128 = 0 | object %128 = 64 | buffer %128 = 0 | buffer %128 = 64 |
|---:|---|---|---|---|---|
| 16 B | lle | 306 [15] | 303 [17] | 304 [32] | – |
| 16 B | rigtorp | 347 [17] | 307 [15] | 333 [12] | 325 [20] |
| 16 B | boost | 358 [14] | 349 [18] | 352 [18] | 355 [14] |
| 64 B | lle | 366 [16] | 366 [16] | 366 [32] | – |
| 64 B | rigtorp | 429 [17] | 366 [15] | 393 [10] | 403 [22] |
| 64 B | boost | 413 [20] | 401 [12] | 405 [16] | 412 [16] |
| 256 B | lle | 528 [17] | 526 [15] | 527 [32] | – |
| 256 B | rigtorp | 599 [17] | 536 [15] | 571 [12] | 568 [20] |
| 256 B | boost | 581 [13] | 577 [19] | 577 [19] | 580 [13] |
| 1024 B | lle | 893 [15] | 893 [17] | 893 [32] | – |
| 1024 B | rigtorp | 916 [16] | 846 [16] | 891 [19] | 867 [13] |
| 1024 B | boost | 895 [13] | 885 [19] | 886 [17] | 893 [15] |

## Throughput (M events/s): mean ± 95% CI across runs

| Payload | Slots | Runs | lle | rigtorp | boost |
|---:|---:|---:|---|---|---|
| 16 B | 64 | 10 | 41.79 ± 8.95 | 18.23 ± 0.89 | 14.59 ± 0.64 |
| 16 B | 1024 | 10 | 96.04 ± 5.78 | 18.64 ± 1.29 | 14.15 ± 0.78 |
| 16 B | 16384 | 10 | 93.20 ± 7.56 | 18.79 ± 1.16 | 15.35 ± 1.08 |
| 64 B | 64 | 10 | 15.45 ± 0.46 | 15.39 ± 0.59 | 14.02 ± 0.51 |
| 64 B | 1024 | 10 | 14.67 ± 0.20 | 15.71 ± 0.41 | 13.38 ± 0.59 |
| 64 B | 16384 | 10 | 14.78 ± 0.40 | 16.29 ± 0.60 | 14.11 ± 0.46 |
| 256 B | 64 | 10 | 16.63 ± 0.24 | 9.85 ± 0.18 | 9.23 ± 0.08 |
| 256 B | 1024 | 10 | 26.89 ± 0.50 | 9.95 ± 0.10 | 9.54 ± 0.16 |
| 256 B | 16384 | 10 | 27.37 ± 0.07 | 10.22 ± 0.13 | 9.43 ± 0.15 |
| 1024 B | 64 | 10 | 9.94 ± 0.22 | 8.63 ± 0.28 | 6.45 ± 0.31 |
| 1024 B | 1024 | 10 | 13.41 ± 0.14 | 9.01 ± 0.07 | 6.95 ± 0.12 |
| 1024 B | 16384 | 10 | 11.76 ± 0.10 | 6.86 ± 0.05 | 5.70 ± 0.05 |

## Throughput (M events/s): median (min–max) across runs

| Payload | Slots | Runs | lle | rigtorp | boost |
|---:|---:|---:|---|---|---|
| 16 B | 64 | 10 | 47.47 (24.34–56.49) | 18.58 (16.01–19.47) | 14.95 (13.18–15.51) |
| 16 B | 1024 | 10 | 98.51 (73.57–100.54) | 18.78 (16.06–21.00) | 13.52 (13.02–15.78) |
| 16 B | 16384 | 10 | 96.33 (78.40–103.36) | 18.39 (17.11–21.64) | 15.58 (13.05–18.00) |
| 64 B | 64 | 10 | 15.45 (14.37–16.59) | 15.19 (14.34–17.19) | 14.01 (12.89–15.20) |
| 64 B | 1024 | 10 | 14.61 (14.05–14.98) | 15.87 (14.80–16.29) | 13.20 (12.51–14.71) |
| 64 B | 16384 | 10 | 14.86 (14.02–15.59) | 16.14 (15.21–17.83) | 14.19 (12.71–14.88) |
| 256 B | 64 | 10 | 16.57 (16.15–17.38) | 9.91 (9.40–10.20) | 9.26 (9.03–9.37) |
| 256 B | 1024 | 10 | 27.07 (24.98–27.34) | 9.91 (9.81–10.18) | 9.54 (9.05–9.84) |
| 256 B | 16384 | 10 | 27.40 (27.11–27.44) | 10.18 (9.94–10.48) | 9.46 (9.00–9.79) |
| 1024 B | 64 | 10 | 9.84 (9.66–10.69) | 8.54 (8.13–9.20) | 6.58 (5.67–6.92) |
| 1024 B | 1024 | 10 | 13.46 (13.01–13.61) | 9.01 (8.88–9.13) | 7.01 (6.67–7.22) |
| 1024 B | 16384 | 10 | 11.80 (11.41–11.94) | 6.84 (6.77–6.97) | 5.68 (5.63–5.86) |

## Latency benchmark header (first process)

## SPSC ping-pong latency (round trip)

- initiator cpu 2 (package 0, core 2), echo cpu 4 (package 0, core 4)
- compiler: 15.2.0; build: GNU 15.2.0 Release -O3 -DNDEBUG
- TSC: 2592.002 MHz (calibration windows 2592.001..2592.002 MHz)
- timer overhead per sample (fenced rdtsc pair): min 14.7 ns, median 17.0 ns
- for reference, steady_clock::now() pair: min 22.0 ns, median 25.0 ns
- samples 2000000 per case after 200000 warm-up round trips; reps 1; capacity 1024; spin busy
- layout: offsets within a page randomized, seed 257400863
- values are round-trip ns and include the timer overhead above; not halved
