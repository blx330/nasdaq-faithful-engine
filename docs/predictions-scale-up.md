# Predictions for the scale-up (written 2026-11-22, before any full-day or multi-day run)

Status: nothing below has been checked against data beyond the 5-symbol extract
of 2020-01-30 (13,277,515 messages, 0 unexplained after entry-order priority).
The full day, the 20-symbol panel and the extra days have not been run.
This file is committed before they are, so the commit time is the evidence that
these are predictions and not descriptions.

## Definitions (fixed now, so they cannot drift to fit the result)

- Continuous events: all inferred aggressor events minus the `cross` class
  (same as `nfe_fidelity` prints).
- Unexplained rate = `unexplained` / continuous events. Also reported per symbol
  and per liquidity tier, because an event-weighted total is dominated by liquid names.
- Late insert: an Add whose reference number is lower than the newest order
  already resting at its price level (`book.late_inserts`).
- Open time of a symbol: timestamp of its opening cross (Q message, type O).
  Offsets are measured from that, and also from 09:30:00, because delayed-open
  symbols make the two differ.
- Liquidity tiers: by total ITCH messages per symbol on 2020-01-30, ranked,
  tier 1 = rank 1-50, tier 2 = 51-500, tier 3 = 501-2000, tier 4 = rank 2001 and
  below with at least 2,000 messages.

## P1. Late inserts (your claim: only in the first ~40 s after the open)

- Measured from each symbol's own opening cross: at least 98% of late inserts fall
  within 45 s. My confidence: 70%.
- Measured in wall-clock time (everything inside 09:30:00 to 09:30:45): I expect this
  to FAIL as stated, 80% confidence, because symbols with a delayed open (halted
  opens, IPOs, thin names) have their own cross after 09:30, and releases follow it.
- Late inserts outside the window of the symbol's own open or reopen: under 1% of all
  late inserts, concentrated in symbols with an intraday halt or pause.
- Late events (fills of a late-released order) occur all day, up to the close. That
  already happened in the extract (72 of the 217 were later than the first two
  minutes), and is not a violation: the claim is about when the insert happens,
  not when the order is later hit.
- Total late inserts on the full day: point estimate 600,000, 80% interval
  250,000 to 2,000,000. The extract had 29,250 for its 5 symbols.

## P2. Unexplained (your claim: stays under 0.1%)

- Full day, event-weighted: point estimate 0.04%, 80% interval 0.005% to 0.25%.
  P(under 0.1%) = 65%.
- P(exactly 0 unexplained on the full day) = 3%. Zero over ~8,000 symbols and
  hundreds of millions of messages would surprise me, and I would first suspect a
  bug in the classifier (something is being absorbed into another class) before
  celebrating.
- The aggregate is the wrong test on its own. By tier I expect the rate to rise as
  liquidity falls: tier 4 rate between 3x and 10x the tier 1 rate.
- Concentration: at least 80% of unexplained events sit in fewer than 2% of
  symbols. Candidates: halted or LULD-paused symbols, delayed opens, sub-dollar
  prices, IPO days.
- Any symbol with more than 1% unexplained gets traced individually.

## P3. Illiquid symbols (your guess: more hidden liquidity, thinner queues)

I split this into three separate claims, because they can fail independently:

1. Thinner queues: median orders ahead of a new order at the touch is at most 3 in
   tier 4 and at least 20 in tier 1. Confidence 85%.
2. More hidden liquidity: share of continuous events in `match_hidden`, `hidden_only`
   and `non_displayed` combined is at least 1.5x higher in tier 4 than in tier 1.
   Confidence 60%. I am less sure than you seem to be: in thin books a lot of the
   hidden share is midpoint and odd-lot flow, and large liquid names also carry
   heavy reserve and hidden volume. The extract already shows `hidden_only` is 17%
   of events in the five big names.
3. Fidelity: exact match among displayed events is lower in tier 4 than tier 1
   (extract: 93.96%), by 2 to 10 points. Confidence 55%. The opposing force is that
   a one-order touch makes FIFO trivially right.
- Late events as a share of events is lower in tier 4 (fewer pre-open orders).
  Confidence 75%.
- Queue study (fill probability by queue decile) will be near degenerate in tier 4:
  too few distinct queue positions for ten deciles. Report with the tick-constrained
  split and do not claim a queue-position gradient there.

## P4. Release delay (1 s vs 11 s after the cross)

I could not find the 1 s vs 11 s figure in the repo, so I treat it as a hypothesis,
not a result. Metric: per symbol, median Add timestamp of its late-inserted orders
minus the symbol's open cross.

- Release offsets are not one smooth distribution: distinct modes exist. Confidence 65%.
- Modes within 0.5 s of 1 s and 11 s: 45%.
- What separates the modes is the listing venue or security type available in the
  Stock Directory message (market category, ETP flag), not liquidity. At least 85% of
  symbols in a mode share one market category; liquidity tier explains under 20% of
  the variance in delay. Confidence 55%.
- Alternative I will test explicitly: the delay is a function of when the symbol's own
  cross printed, not a fixed offset. Measuring from both clocks separates these.
- Nothing here gets called a rule until it is checked against Nasdaq rule text or a
  second source. The extract finding was inferred from the feed, and still is.

## P5. Resources

- Peak memory of `nfe_fidelity` on the full day: under 2.5 GB. Runtime: 3 to 15 minutes
  on a laptop with the file in page cache (the extract ran at about 7M msgs/s;
  I expect 2x to 5x slower with 8,000 symbols).
- Fast book (paged lookup) still slower than baseline on the full day, if the sparse
  reference-number cause is right: 70%.

## How I will score this

Each claim is marked right, wrong, or untestable in `docs/scale-up-results.md`, with
the number next to the prediction. Wrong predictions stay in this file unedited.
Changes to definitions after the first full-day run are logged there as changes.
