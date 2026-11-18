#!/usr/bin/env bash
# Benchmark under controlled conditions.
#
#   scripts/bench.sh <itch-file> [json-out] [runs]
#
# Pins to one core, asks for the performance governor if cpupower is
# available, and wraps the run in `perf stat` when hardware counters are
# readable. Without counters it still runs and says so: check this in phase 0
# on the machine you will benchmark on.
set -euo pipefail
FILE=$1
OUT=${2:-results/bench.json}
RUNS=${3:-10}
BUILD=${BUILD:-build}
CORE=${CORE:-$(( $(nproc) - 1 ))}

if command -v cpupower >/dev/null 2>&1; then
  sudo -n cpupower frequency-set -g performance >/dev/null 2>&1 || echo "note: could not set the performance governor"
fi
PIN=""
command -v taskset >/dev/null 2>&1 && PIN="taskset -c $CORE"

EVENTS=cycles,instructions,branch-misses,cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses
if command -v perf >/dev/null 2>&1 && perf stat -e cycles true >/dev/null 2>&1; then
  for book in baseline fast; do
    $PIN perf stat -x, -e "$EVENTS" -o "${OUT%.json}.perf_$book.csv" \
      "$BUILD"/nfe_bench "$FILE" --runs 1 --book $book >/dev/null
  done
  echo "perf counters written next to $OUT"
else
  echo "note: no hardware counter access here (perf missing or perf_event_paranoid); throughput only"
fi
$PIN "$BUILD"/nfe_bench "$FILE" --runs "$RUNS" --json "$OUT"
