# Engineering log

Dated entries: what broke, the hypothesis, how it was diagnosed. Keep adding
to this by hand; it is the authorship evidence.

## 2026-11-17: first build

**Data access.** The build environment could not reach emi.nasdaq.com, so
everything below ran on synthetic days from `sim/itch_sim.py`. The real-day
run is the next step and will produce its own entries.

**Fidelity on synthetic data: 0 unexplained on the first run.** Expected,
and weak evidence: the simulator and the engine apply the same priority
rules. The value of synthetic data is the ground truth for aggressor
grouping, not the match rate.

**Posted remainders lost after hidden-only fills.** Ground-truth scoring
showed 77 of 339 posted remainders not recovered. All 77 were events whose
only executions were Trade (P) messages, so the resting side was unknown and
the remainder rule (an Add on the side opposite the resting side) could not
fire. P's buy/sell field is not a reliable resting side on the real feed, so
it is still not used to split events. Fix: a passive order can never rest at
or through a price that just traded against hidden liquidity, so an Add at
that timestamp on the far side of the P prices must be the aggressor's.
After: 339 of 339 recovered, quantity correct on 100% of one-to-one events.

**Mutant matrix, first run: 24 of 28.** Four survivors, four different reasons:

1. `unlink_tail_stale` was an equivalent mutant. The tail is only left stale
   when the last order of a level leaves, and that level is then erased, so
   no validator can ever see it. Replaced with a real bug (tail not moved
   back when the newest of several orders leaves); all three validators
   catch the replacement.
2. `duplicate_ref_accepted` and `overfill_unchecked` survived because no
   input ever exercised them: the protocol driver rejects duplicate ids
   before the book sees them, and neither real nor synthetic feeds contain
   duplicates or overfills. Added a negative control: `sim/corrupt_feed.py`
   injects one duplicate Add, one overfill and one unknown Delete, and the
   replay checker must report exactly those. `overfill_unchecked` still
   survived (the injected overfill was counted, but by a later message),
   until the control also required the book to stay uncrossed afterwards.
3. `remainder_side_wrong` survived because a missing remainder does not
   change the prediction: with the limit set to the worst fill and the size
   to the fills, the engine still predicts the same fills. Added ground-truth
   scoring of the inferred aggressors to the replay validator; it now fails
   on any change.

**Simulator retune broke a detection.** After making the synthetic book
livelier (below), `halt_state_ignored` survived: the new halted symbol never
crossed, so treating halts as continuous trading changed nothing. Halted and
quote-only periods now receive crossing orders, as pre-open does. Lesson: the
negative space of a test input matters as much as the input.

**Synthetic touch never cleared.** First queue-study run: 4,146 quote
changes in a whole day, and orders waiting 15 minutes at the touch. Pre-open
accumulation made levels huge and cancels were uniformly random. Halved
pre-open, made half of cancels target the order most at risk from fair
value, and added aggressors that sweep the whole touch. Quote changes rose to
about 54,000 per day.

**Queue deciles were broken by ties.** 46% of orders at the touch set a new
best price and have nobody ahead, which made decile 1 and 2 empty after
ranking. They are now a separate group ("new best"), and deciles rank only
orders that joined an existing level. This is also the right economics: a
price improvement is not a queue position.

**Fast book, change 1: paged direct-indexed order lookup.** Prediction:
order lookup is a large share of book time because every E, X, D and U
message hashes a 64-bit reference; a direct index should cut book-only time
by 20-30%. Measured on the 3.2M-message synthetic day, 5 runs: book-only
19.2 to 22.4 M op/s (+16%), replay 17.7 to 19.5 M msg/s, p50 latency 69 to
50 ns. Smaller than predicted. No hardware counters on this machine, so the
cache-miss explanation is untested; repeat with `perf stat` on the benchmark
machine before claiming why.

**CppTrader baseline.** Built `cpptrader-performance-market_manager` and ran
it on the same file: 13.6 M msg/s median of 5, against 19.5 here (1.44x).
Not like for like: CppTrader's market manager also runs handler callbacks and
maintains per-level update events. Its optimized variants preallocate 300M
order slots and ran out of memory on this 2-core sandbox.

## 2026-11-18: first real day (2020-01-30, extract of AAPL, MSFT, SPY, QQQ, F)

13,277,515 messages. Reconstruction clean: 0 unknown references, 0
overfills, 0 crossed books in continuous trading.

**Fidelity: 2,240 unexplained of 191,798 continuous events (1.17%), above
the 1% kill criterion.** Every one falls into two patterns:

1. **2,024 (90%): the posted remainder is priced less aggressively than the
   aggressor's own fills.** Example, MSFT 09:30:00.485: a buy executes 99
   shares at 173.97, then an Add for the remaining share is posted at
   173.96. Read as the limit, 173.96 predicts no fills at all. Hypothesis:
   Nasdaq's Price to Comply handling, which reprices a remainder that would
   lock or cross another market's quote. Fix: a remainder that is less
   aggressive than the fills is not the limit; predict with the worst fill
   and classify a match as `remainder_repriced`. To check: does the posted
   price sit one tick inside the away quote? That needs NBBO data.
2. **216 (10%): queue order differs; the feed fills an older reference
   first.** 145 of them in the first two minutes after the open. The same
   few resting orders (refs 4226041, 5714789, 10183329) sit at the head of
   our queue but are skipped event after event, while orders with lower
   reference numbers that arrived later in the feed fill first. Hypothesis:
   after the opening cross, priority follows original entry order (the
   reference number), not the order in which Add messages were released.
   Not yet fixed; next step is to find when those skipped orders were added
   and what happened to them in the cross.

**Fast book 4x slower than the baseline on the extract** (2.1 vs 8.3 M
msg/s). Prediction was +20 to 30%. Cause: an extract keeps 5 of ~8,000
symbols, so its reference numbers are sparse across the day's whole range,
and nearly every Add allocates and zeroes a fresh 229 KB page that is freed
again soon after. The design assumed dense references; it must be judged on
the full day, and the paging should be compared against an open-addressing
hash map, which has no sparsity problem. Kept in the log as a failed
prediction.

**Latency percentiles read 0 on Apple silicon**: the ARM timer ticks every
~42 ns. The benchmark now says so; latency numbers come from x86 Linux only.
