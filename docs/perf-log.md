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
