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
