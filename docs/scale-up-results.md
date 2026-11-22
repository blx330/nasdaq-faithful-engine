# Scale-up results: full 2020-01-30 day (2026-11-22)

Predictions were committed first (`docs/predictions-scale-up.md`, commit e13fcba). This file scores them.
Multi-day runs are NOT done: see "Not done".

## What was run

- The whole 2020-01-30 file: 423,285,709 messages, 8,915 symbols, 0 unknown or malformed messages.
- Run in 4 symbol shards (round robin by message-count rank, `nfe_extract`), each through `nfe_replay --strict`,
  `nfe_fidelity` and the new `nfe_lateadds`. Per-symbol books are independent, shard message counts add up exactly
  to the file (423,285,700 symbol messages plus the 9 market-wide ones), and the same build reproduces the earlier
  5-symbol extract number for number. It is still not one single process over the whole file; see caveats.
- Results in `results/scale-up/`. Analysis: `analysis/scale_up.py`. Panel pipeline: `scripts/panel_day.sh`.

## Headline

| Check | Result |
| --- | --- |
| Reconstruction | 0 unknown refs, 0 overfills, 0 crossed books in continuous trading, all four shards |
| Events | 7,003,063 (6,874,975 continuous) |
| Unexplained | **0** (also 0 in every liquidity tier) |
| Exact match, displayed-book events | 94.0% (tier 1) to 97.0% (tier 5) |
| Late inserts (Add placed ahead of a resting order) | 316,692 in 6,564 of 8,915 symbols |
| Late events (fills of a late-released order) | 4,608 |

## The zero needs a qualification (new finding)

`unexplained` can only be reached by an event with no Trade (P) message and no printable C message.
Any event with one is filed as `non_displayed` or `nondisplay_price` without checking its displayed fills against the
queue. That is 242,173 events (3.5% of continuous events) where a queue-order error could hide.

I added an audit (`audit_displayed` in `fidelity.hpp`, additive, class counts unchanged): re-predict those events from
the displayed shares only, then explain any mismatch constructively.

- 92,645 of the 242,173 do NOT match a displayed-only prediction (1,885 non_displayed, 90,760 nondisplay_price).
- Cause, from tracing MSFT at 09:30:00.58: orders displayed at one price execute through printable C messages at a
  better price (for example displayed 173.98, executed 173.97). They behave as if they sat at the better price, so
  they fill ahead of the displayed queue at the worse level. Consistent with Price to Comply style display sliding;
  that reading is inferred from the feed and not checked against Nasdaq rules.
- Reconstruction test: take those orders out of the displayed queue, fill them first, and the remaining displayed
  fills must reproduce exactly, in order, with execution prices monotone. **All 92,645 reproduce; residual 0.**
- Mismatch categories before reconstruction: price priority 90,594, tie 450, quantity split only 1,601,
  contradiction 0, non-monotone 0.
- The audit can fail: with `--priority feed` (plain FIFO) on the 5-symbol extract its residual rises from 0 to 23.
- Limits: the working price is learned from the feed's own C messages, so this explains these events after the fact;
  it does not predict them. The first version of the audit overstated the problem (2,668 "bad" events on the extract)
  until the working-price reconstruction was added. Records of that are in the engineering log.

So the accurate claim is: 0 unexplained by the original definition, and 0 residual after a stricter displayed-queue
audit, with one mechanism (execution at a better working price than the displayed price) carrying 1.35% of events.

## Predictions scored

| # | Prediction | Result | Score |
| --- | --- | --- | --- |
| P1a | at least 98% of late inserts within 45 s of the symbol's own open marker | 100.0% (max 40.017 s after 09:30:00) | right |
| P1b | the wall-clock window 09:30:00 to 09:30:45 will FAIL (80% confidence) | holds for all 316,692; every open marker prints within 1.26 s of 09:30:00, no delayed opens that day | **wrong** |
| P1c | under 1% of late inserts outside the window | 0 | right |
| P1d | late events occur all day | 4,608; 2,882 in the 9 am hour, 37 in the 4 pm hour, latest 16:37 | right |
| P1e | total late inserts 600,000 (80% interval 250,000 to 2,000,000) | 316,692 | in interval, about half my point estimate |
| P2a | unexplained 0.04% (0.005% to 0.25%) | 0.000% | **wrong** (below the interval) |
| P2b | P(under 0.1%) = 65% | true | right (your claim held) |
| P2c | P(exactly 0) = 3% | it was 0 | **wrong**, which is why I audited it |
| P2d | tier 4 rate 3x to 10x tier 1 | both 0 | untestable |
| P3.1 | thinner queues in tier 4 (at most 3 orders ahead vs 20+) | the tracker records shares, not orders; panel median shares ahead: T1 125, T2 700, T3 0, T4 100, no clean gradient | untestable as stated, not supported |
| P3.2 | hidden share of events in tier 4 at least 1.5x tier 1 | 24.3% vs 22.1% (1.10x). Tier 5 (the 1,982 symbols under 2,000 messages): 34.8% (1.57x); hidden-only 28.4% vs 16.5% | **wrong** for tier 4; your direction is right but only in the thinnest names |
| P3.3 | exact match lower in tier 4, by 2 to 10 points | higher: 96.6% vs 94.0% | **wrong** (opposite sign) |
| P3.4 | late events a smaller share in tier 4 | per 1,000 continuous events: T1 1.76, T2 0.56, T3 0.33, T4 0.89, T5 1.28 | right against tier 1, not monotone |
| P4a | release offsets form distinct modes | yes, two populations | right |
| P4b | modes within 0.5 s of 1 s and 11 s | no 11 s mode; see below | **wrong** |
| P4c | market category, not liquidity, separates them | eta squared of per-symbol median delay: market category 0.45, tier 0.11, ETP flag 0.01 | right (not the 85% purity I stated; that was not computed) |
| P5 | peak memory under 2.5 GB; 3 to 15 min | shard peaks 2.3 GB (fidelity) and 2.7 GB (replay), both counting mapped file pages; one single-process run was cut off by my own tool time limit | untested as a single process |

## Late inserts and the release delay

Two populations, not "1 s vs 11 s":

- **NYSE, NYSE Arca, NYSE American, BATS-listed symbols (market categories N, A, P, Z):** essentially all late inserts
  are one burst at the open marker's own timestamp, 0 to 1.3 s after 09:30:00. For these 99% to 100% of late inserts
  have the same timestamp as the marker. They have no real auction: 5,891 of the 8,915 open markers have zero shares
  and zero price.
- **Nasdaq-listed symbols (Q, G, S):** a staggered drain starting at the cross and ending at 40.0 s after 09:30:00.
  Only 22% of late inserts fall within 5 s of the cross; 69% of AAPL's are in the last 5 s. Batches of about 50 orders
  per release timestamp (AAPL: 278 release timestamps).
- The drain takes longer the more orders are held: median completion 15 s for 20 to 50 held orders, 24 s for 50 to 200,
  31 s for 200 to 1,000, 37 s above 1,000 (correlation of log count with completion time 0.77). That reads as a
  rate-limited release with a hard ceiling at 09:30:40. Release order only weakly follows reference number (rank
  correlation -0.03 to -0.40), so it is not a simple oldest-first queue. This is an observation of the feed, not a
  confirmed rule.
- Late inserts per 1,000 Adds by market category: Q 4.9, G 1.8, S 15.2, N 0.47, P 0.11, Z 0.23, A 3.3.
- The 1 s vs 11 s figure is not supported. I could not find where it came from. The nearest numbers are the open
  marker (median 0.85 s after 09:30:00) and Nasdaq-listed completion times, which spread continuously from 5 s to 40 s.

## Not done

- **Multi-day runs (3 to 5 more days).** The Nasdaq host is blocked from the sandbox shell (403 from the proxy), so
  downloads had to go through your browser, and access to your Downloads folder was refused, so I cannot see or move
  the files. I clicked the 12302019 link once in the built-in browser to test; whether a 3.5 GB file landed in your
  Downloads folder is unknown. Chosen days (same file format as yours, spread across 2019): 01302019 (4.76 GB),
  08302019 (4.08 GB), 12302019 (3.52 GB). The `.md5sum` links return 404 on the server; verify with `gzip -t`.
  Per day: `gunzip -c data/DAY.NASDAQ_ITCH50.gz > data/DAY.NASDAQ_ITCH50 && scripts/panel_day.sh data/DAY.NASDAQ_ITCH50`.
  The script is tested on 2020-01-30 (43 s, 25 symbols, 0 unexplained).
- A single-process run over the whole file, and the Mac's physical RAM: the sandbox shell is a Linux VM with 3.8 GB,
  not the Mac. Disk on the Mac volume: 461 GB total, 98 GB free (79% used), enough for the 13 GB file.
- Performance on the full day (fast book vs baseline): not run.

## Caveats

- Sharding is equivalent per symbol by construction, not proven end to end. Check: run
  `build/nfe_fidelity data/01302020.NASDAQ_ITCH50` on the Mac; expect 7,003,063 events, 0 unexplained, 316,692 late
  inserts (late inserts come from `nfe_lateadds`; `nfe_fidelity` prints its own count of the same thing).
- Tier 5 (symbols under 2,000 messages) and the full-day tier shares are measured on one day.
- I looked at per-symbol late-insert counts of the 5-symbol extract (AAPL 13,409, F 32, SPY 3) after committing the
  predictions and before the full-day run. Nothing in the predictions was changed.
- The panel tier tests use 5 symbols per tier: small samples.
