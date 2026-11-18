# Write-up

Each section is a claim, its evidence, and its limits. Numbers come from
`results/REPORT.md`; until the real-day run is done they are synthetic and
are labelled as such there.

## 1. The book is reconstructed correctly

**Claim.** Replaying a full day leaves no unknown order references, no fill
larger than its order, and no crossed book during continuous trading; closing
cross prices equal the official closes.

**Evidence.** `nfe_replay --strict` (exit code), `crosses.csv` against an
outside close file (`analysis/check_closes.py`), and a negative control: the
same feed with one duplicate Add, one overfill and one unknown Delete
injected must produce exactly those three anomalies (`sim/corrupt_feed.py`).

**Limits.** Broken Trade (B) messages are counted but not reversed; they do
not change the displayed book. A crossed book is allowed outside continuous
trading (pre-open, halts, quote-only periods, before the open or reopening
cross); "continuous" means market hours, state T, and either a cross printed
since entering T or five seconds elapsed.

## 2. The engine reproduces Nasdaq's fills

**Claim.** In matching mode, given the book just before each execution event
and the inferred incoming order, the engine predicts the feed's executions
exactly for X% of displayed-book events, and every other event is assigned
to a named mechanism.

**Method.** `include/nfe/fidelity.hpp`. Consecutive E, C and P messages for
one symbol with the same timestamp and resting side form an event; an Add on
the opposite side at that timestamp is the posted remainder. The aggressor's
size is fills plus remainder; its limit is the remainder's price or the worst
fill. The event is buffered, so the book it is predicted against is exactly
the pre-event book; then the messages are applied.

**Queue priority is entry order, not display order.** Within a price level
the book orders by reference number (`BaselineBook::priority_by_ref`).
Nasdaq assigns reference numbers in increasing order per symbol at
acceptance, so this equals feed order except for orders accepted before the
open and first displayed after the opening cross; the feed fills those ahead
of orders that were displayed earlier but entered later. `--priority feed`
reproduces the plain-FIFO result for comparison.

**Taxonomy.** match; match with hidden also hit; non-displayed liquidity (P in
the event); execution at a non-display price (printable C); cross (outside
continuous trading or non-printable C); grouping ambiguity (matches once
repeated fills of one order are merged, the signature of two aggressors);
hidden only; unexplained. Kill criterion: unexplained above 1% stops feature
work.

**Grouping, tested.** On synthetic days, where every aggressor is known, the
heuristic recovers about 93% of aggressors one-to-one, flags every timestamp
that had several aggressors, recovers every posted remainder, and gets side
and size right on all one-to-one events (`analysis/score_grouping.py`). The
real-data ambiguity rate is reported as the grouping share and the same-
timestamp count.

**Limits.** The check is partly circular: the book was built from
price-time executions and the prediction applies price-time priority. What it
still tests: strict priority among displayed orders, displayed ahead of
hidden at a price, replace losing priority, partial cancel keeping it, and
the limit at which a sweep stops. On the first real day, ordering by
reference number turned all 217 remaining unexplained events into matches
and changed no matching event; it is still a model fitted to one day and
five symbols until it holds on others. Two same-side aggressors that split no
order are indistinguishable from one and are counted as one.

## 3. The test suite catches bugs, measured

**Claim.** 28 of 28 hand-seeded bugs are caught; the matrix shows which
validator caught each and after how many operations.

**Evidence.** `mutants/`, `results/mutant_matrix.md`. Validators: differential
fuzzing against the OCaml executable spec (written first, with its own QCheck
properties), C++ property tests with invariants after every operation, and
ITCH replay (strict replay, fidelity, ground-truth scoring, negative control).
Ten bugs are caught by exactly one validator, which is the argument for
having three. libFuzzer runs on the parser and the engine under ASan/UBSan.

**Limits.** Fuzzer detection is reported at segment granularity (200
operations). The seeded bugs are mine; real bugs may be subtler.

## 4. What queue position is worth

**Question.** For displayed orders added at the best bid or ask during
continuous trading: how does the probability of a fill within 1, 10 and 60 s,
and the markout after a fill, depend on shares ahead at arrival?

**Hypotheses, written before running.** H1: fill probability falls
monotonically with shares ahead. H2: back-of-queue fills have worse markouts,
because they tend to fill only when the whole level is swept. H3: both
effects are larger in tick-constrained symbols.

**Design.** Orders that set a new best price have nobody ahead and form their
own group; deciles rank orders that joined an existing level, within each
symbol-day. Fill probability is reported unconditionally and conditional on
the order not having been cancelled or replaced first. Markout: the move of
Nasdaq's mid against the order, in bps of the pre-fill mid; net capture is the
half-spread earned minus the markout. 95% intervals resample whole
symbol-days (1,000 draws).

**Limits.** The mid is Nasdaq's, not the NBBO. Cancels are endogenous. Hidden
liquidity can fill ahead of or behind displayed orders. The public days are
few and some are years old. Shares ahead is measured at arrival only. Queue
position correlates with order size and time of day; a regression with
those controls is the next step before claiming causality.

**Prior work.** Moallemi, C. C. and Yuan, K., "A Model for Queue Position
Valuation in a Limit Order Book", Columbia Business School working paper
(SSRN, 2017). They model queue value from informational and queueing effects,
calibrate it empirically, and find it can be of the order of the half
spread. This study is an empirical replication-in-spirit on public data, not
a new model; say so. _Confirm the final published version before citing._

**Synthetic results are not findings.** The simulator's aggressors are
informed by construction, so its markouts say nothing about Nasdaq.

## 5. Performance

**Claim.** Full-day replay throughput of T M msg/s, R times CppTrader's
market manager on the same machine and file, with each kept change tied to a
measured counter delta. See `docs/perf-log.md`.

## What I got wrong

See the engineering log; the highlights so far: an equivalent mutant counted
as a test failure; two seeded bugs that no input could reach; a fidelity
check blind to a missing remainder; a simulator change that silently removed
a validator's power; queue deciles broken by ties at zero.
