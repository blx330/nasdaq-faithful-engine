# Write-up

Each section is a claim, its evidence, and its limits. Real-data numbers are
from Nasdaq's public ITCH sample days and live in `docs/scale-up-results.md`,
`docs/queue-results.md` and `results/`; `results/REPORT.md` is the synthetic
run that CI reproduces and is labelled as such.

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
and the inferred incoming order, plain price-time FIFO predicts the feed's
executions exactly for 95.3% of the 5,635,687 displayed-book events on the full
2020-01-30 day. With two further rules (entry-order priority, and orders that
execute at a better price than displayed filling first) no event is left
unexplained and the displayed-queue audit has no residual, on that day and,
unchanged, on a 25-symbol panel on three other days.

**Results.** `docs/scale-up-results.md`: full day 7,003,063 events, 6,874,975
continuous, 0 unknown references, 0 unexplained; audit 92,645 events
reproduced, residual 0; with feed-order priority instead, 2,844 unexplained and
an audit residual of 252. Panel days: 0 and 0 with entry order; 83 to 270
unexplained and 8 to 29 residual with feed order.

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
and changed no matching event. It was fitted on one day and five symbols; it
has since held on the full market that day and on three other days.

The zero is weaker than it looks in two ways. First, `unexplained` can only be
reached by an event with no P message and no printable C, so 242,173 events
were originally filed under a mechanism without checking their displayed fills.
The audit re-predicts those from displayed shares: 92,645 fail a displayed-only
FIFO prediction, and all of them are reproduced once orders that executed at a
better price than displayed are filled first. Second, that rule takes the
working price from the event's own C messages, so it explains those executions
after the fact; it does not predict them from the book.

**Rule text.** The better-price mechanism is consistent with the Price to
Comply order type, Nasdaq Equity 4, Rule 4702(b)(1): such an order is
displayed one minimum increment away from a price that would lock or cross a
protected quotation and is ranked at a non-displayed price (SEC release
34-106936 on that rule gives the example of a buy order ranked at $11 and
displayed at $10.99). The repriced remainder is the same order type seen from the
aggressor's side. For the opening release, Rule 4752 defines orders with a
market-hours time in force entered before the open ("Early Market Hours
Orders", 4752(a)(7)), which fits orders accepted early and made active at
09:30; I found no rule text for the order or pace in which they reach the
book, so the 40-second release queue is inferred from the feed. Two same-side aggressors that split no
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

**Answer** (`docs/queue-results.md`; 25 symbols, 4 days, 99 symbol-days,
14.4M orders, 915,002 fills). Front-decile joiners fill within 10 s 3.9 times
as often as back-decile joiners (95% interval 3.1 to 5.1), monotone across
deciles, and the shares-ahead coefficient stays negative with controls for
imbalance, spread, size and hour, at a little over half its raw size. There is
no back-of-queue markout penalty: -0.70 bps at 1 s (interval -1.60 to +0.09),
negative in tick-constrained symbols and positive in wider-spread ones. 2.5%
of fills are at a better price than displayed and bypass the displayed queue.

**Predictions.** Nine, committed before the run (`docs/predictions-queue.md`).
Right: the size and direction of the fill effect, the better-price share, the
effect surviving controls. Wrong: back-of-queue fills having worse markouts,
and tier 4 being too thin to show an effect. Not supported: new-best orders
having worse markouts than joiners.

**Design.** Orders that set a new best price have nobody ahead and form their
own group; deciles rank orders that joined an existing level, within each
symbol-day. Orders arriving before 09:31:00 are dropped because the opening
release places orders by entry order. Orders with a better-price fill are
tagged and results are shown with and without them. Markout: the move of
Nasdaq's mid against the order, in bps of the pre-fill mid. 95% intervals
resample whole symbol-days (1,000 draws).

**Limits.** The mid is Nasdaq's, not the NBBO. Cancels are endogenous. Shares
ahead is measured at arrival only and mixes "early at a fresh level" with
"late at a dying level", which is the likely reason the markout sign depends
on the regime. Four days, 25 symbols, dominated by the largest names. The
regression is a control exercise, not a causal estimate.

**Prior work.** Moallemi, C. C. and Yuan, K., "A Model for Queue Position
Valuation in a Limit Order Book", Columbia Business School working paper
(SSRN, 2017). They model queue value from informational and queueing effects
and calibrate it empirically. This study is a descriptive measurement on
public data, not a model.

**Synthetic results are not findings.** `results/queue/` is the simulator run
that CI reproduces; its aggressors are informed by construction.

## 5. Performance

**Claim, and it is a small one.** The baseline book replays a real 25-symbol
extract (2019-12-30, 12.0M messages) at about 6.8M messages/s, and the full
fidelity pipeline does the whole 423M-message day in 240 s in one process.
The paged order lookup in `fast_book.hpp` is 16% faster than the baseline on
a dense 8-symbol synthetic day and about 6 times slower on the real extract.
`docs/perf-log.md` has the numbers, the machines and the diagnosis. The 1.44x
figure against CppTrader was measured on the synthetic day against a tool
that does more per message, so it is not quoted as a result.

## What I got wrong

See the engineering log; the highlights so far: an equivalent mutant counted
as a test failure; two seeded bugs that no input could reach; a fidelity
check blind to a missing remainder; a simulator change that silently removed
a validator's power; queue deciles broken by ties at zero.
