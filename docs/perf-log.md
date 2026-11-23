# Performance log

One change per commit. Before each: a written prediction. After: throughput
and `perf stat` counters. Reverted changes stay here.

Measurement: `scripts/bench.sh FILE` pins one core, sets the performance
governor when it can, discards a warm-up run, reports mean ± sd over 10 runs,
and wraps `perf stat` when counters are readable. Latency is per book
operation, timed with `rdtsc` and fenced; the timer's own cost is reported
separately and included in the percentiles.

| # | Change | Prediction | Result | Kept |
| --- | --- | --- | --- | --- |
| 0 | Baseline: `std::map` levels, `std::unordered_map` orders | - | 17.7 M msg/s replay, 19.2 M op/s book-only, p50 69 ns (synthetic 3.2M msgs, 2-core sandbox, no counters) | - |
| 1 | Paged direct-indexed order lookup (`fast_book.hpp`) | book-only time -20 to -30%: no hashing, refs nearly sequential | book-only +16% (22.4 M op/s), replay +10%, p50 50 ns; counters not available | yes |

| 1r | Same change, real data: 2019-12-30 panel extract, 12,006,327 messages, 25 symbols (2026-11-23) | none written first; the 5-symbol extract had already shown the fast book slower | baseline 6.80 ± 1.71 M msg/s replay, 6.67 ± 3.10 M op/s book-only; fast 1.13 ± 0.14 replay, 1.29 ± 0.22 book-only. Fast book about 6x slower. Linux VM (4 vCPU, 3.9 GB) on an M-series MacBook Air, 5 runs, not pinned, no counters; parse-only 191 M msg/s. `results/bench-real-panel-12302019.json` | no, see below |

**Why the paged lookup loses on real data (diagnosis, not yet profiled).** A page holds 4,096 `Order` slots of 64
bytes, so 256 KB, zero-filled when allocated. On the synthetic day reference numbers are dense, so a page is filled
and then freed. On a real extract the symbols kept own a small fraction of the reference numbers, so most pages hold
a handful of live orders: nearly every Add touches a cold 256 KB page, and one long-lived order pins its whole page.
The full day is denser than an extract but has the same long-lived-order problem. The design assumed dense
references and that assumption is false for real feeds. The run-to-run spread above is large (VM, not pinned), but
not 6x large. Latency percentiles on this machine are quantized to 42 ns and are not reported.

**What would actually help, in order:** an open-addressing hash map keyed by reference number (no page pinning);
`Order` cut to the hot fields with 32-bit links from a pool; and replacing the `std::map` of levels, which neither
design has touched and which is where a real book spends its time. None of these is done.

**Next candidates, untested:** open-addressing hash map as an alternative to
paging (fewer pages kept alive by long-lived orders); order slot layout (hot
fields in one cache line); pooled level nodes; prefetch on lookup; a dense
price window around the touch with a fallback map for stub quotes and
sub-penny prices.

**External baseline.** `scripts/cpptrader_baseline.sh` builds
CppTrader's `cpptrader-performance-market_manager` and replays the same file.
It does more per message than `nfe_replay`: market-update callbacks and
level-update events for every change. Report the ratio with that caveat, and
note that its optimized variants preallocate 300M order slots.

**Repeat everything on the benchmark machine** with the real full day before
quoting a number: the synthetic day has 8 symbols and fits in cache.
