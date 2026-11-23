# Queue study: predictions

Written 2026-11-23, before running the queue study on real data. The only queue
numbers seen so far are from the synthetic days (front/back fill ratio about 2.7
at 10 s, no markout difference), which say nothing about Nasdaq.

Sample: the 25-symbol panel on four days (2019-01-30, 2019-08-30, 2019-12-30,
2020-01-30), displayed orders added at the best bid or ask in continuous trading,
arriving 09:31:00 to 15:59:00. Deciles rank shares ahead at arrival among orders
that joined an existing level, within each symbol-day.

| # | Prediction | Confidence |
| --- | --- | --- |
| 1 | Fill within 10 s: front decile over back decile is between 1.5x and 4x | 70% |
| 1a | Direction only: front decile fills more often than back decile | 95% |
| 2 | 1 s markout: back-of-queue fills are worse (more adverse) than front-of-queue fills | 60% |
| 2a | The 95% interval for that difference excludes zero | 40% |
| 3 | Orders that set a new best price fill within 10 s more often than front-decile joiners | 60% |
| 3a | Their 1 s markout is worse than joiners' | 70% |
| 4 | Fills at a better price than the order's displayed price are 1% to 5% of tracked fills | 60% |
| 5 | The shares-ahead effect on a 10 s fill stays negative after controlling for imbalance, spread, order size, time of day and symbol-day | 90% |
| 6 | Tier 4 symbols: queues too thin, the front/back interval includes 1 (no effect detectable) | 70% |

**Null result.** The front/back fill ratio interval includes 1.

**Fixed before running.** Orders arriving before 09:31:00 are dropped (the opening
release places orders by entry order for the first 40 s or so, so shares ahead at
arrival is not well defined there). Orders with any fill at a price different from
their displayed price are reported as their own group, and the main result is shown
with and without them. Intervals resample whole symbol-days.
