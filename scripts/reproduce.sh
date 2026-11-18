#!/usr/bin/env bash
# Regenerate every number and chart.
#
#   scripts/reproduce.sh                       # synthetic days (what CI runs)
#   ITCH="data/01302020.NASDAQ_ITCH50 ..." SYMBOLS=AAPL,MSFT,... scripts/reproduce.sh
#
# Real days: put uncompressed ITCH files under data/. Optional official closes
# for a day go in data/<file-name>.closes.csv (columns symbol,close).
# The mutant matrix always runs on synthetic data: it needs ground truth.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build}
RESULTS=${RESULTS:-results}
EVENTS=${EVENTS:-200000}
MUTANT_OPS=${MUTANT_OPS:-200000}
BENCH_RUNS=${BENCH_RUNS:-10}
mkdir -p data out "$RESULTS"

echo "== build"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN)" >/dev/null
(cd spec && dune build 2>&1)

echo "== synthetic days (ground truth, negative control, mutants)"
for s in 1 2 3; do
  [ -f data/synth$s.itch ] || python3 sim/itch_sim.py --out data/synth$s.itch --truth data/truth$s.jsonl \
      --closes data/synth$s.itch.closes.csv --events "$EVENTS" --seed $s >/dev/null
done
python3 sim/corrupt_feed.py data/synth1.itch data/corrupt.itch data/corrupt_expected.json >/dev/null

if [ -z "${ITCH:-}" ]; then
  FILES="data/synth1.itch data/synth2.itch data/synth3.itch"
  LABEL="synthetic days"
else
  FILES="$ITCH"
  LABEL="Nasdaq ITCH: $(for f in $ITCH; do basename "$f"; done | tr '\n' ' ')"
fi

DAYS=()
for f in $FILES; do
  name=$(basename "$f")
  d=out/$name
  DAYS+=("$d")
  echo "== $name: replay, fidelity, queue tracking"
  "$BUILD"/nfe_replay "$f" --out "$d" --strict
  "$BUILD"/nfe_fidelity "$f" --out "$d" | head -1
  if [ -f "$f.closes.csv" ]; then
    python3 analysis/check_closes.py "$d/crosses.csv" "$f.closes.csv" --json "$d/closes.json" >/dev/null || true
  fi
  "$BUILD"/nfe_queue "$f" --out "$d/queue" --day "$name" ${SYMBOLS:+--symbols "$SYMBOLS"}
  t=data/truth${name#synth}; t=${t%.itch}.jsonl
  if [[ "$name" == synth* ]] && [ -f "$t" ]; then
    python3 analysis/score_grouping.py "$d/events.csv" "$t" --json "$RESULTS/grouping_score_$name.json" >/dev/null
  fi
done

echo "== queue position study"
python3 analysis/queue_study.py $(for d in "${DAYS[@]}"; do echo "$d/queue"; done) --out "$RESULTS/queue" >/dev/null

echo "== performance"
BENCH_FILE=${BENCH_FILE:-$(echo $FILES | cut -d' ' -f1)}
if [ -z "${ITCH:-}" ]; then
  [ -f data/synth_big.itch ] || python3 sim/itch_sim.py --out data/synth_big.itch --truth /dev/null \
      --closes /dev/null --events 3000000 --seed 9 >/dev/null
  BENCH_FILE=data/synth_big.itch
fi
scripts/bench.sh "$BENCH_FILE" "$RESULTS/bench.json" "$BENCH_RUNS"
if [ "${CPPTRADER:-0}" = 1 ]; then scripts/cpptrader_baseline.sh "$BENCH_FILE" "$RESULTS/cpptrader.json"; fi

echo "== mutant matrix"
python3 mutants/make_patches.py >/dev/null
python3 mutants/run_mutants.py --itch data/synth1.itch --truth data/truth1.jsonl --corrupt data/corrupt.itch \
    --corrupt-expected data/corrupt_expected.json --ops "$MUTANT_OPS" --jobs "$(getconf _NPROCESSORS_ONLN)" --out "$RESULTS" \
    --build "$BUILD" | tail -2

echo "== report"
python3 analysis/report.py --results "$RESULTS" --days "${DAYS[@]}" --label "$LABEL"
echo "wrote $RESULTS/REPORT.md"
