#!/usr/bin/env bash
# Run the 25-symbol panel on one uncompressed ITCH day.
#   gunzip -c data/DAY.NASDAQ_ITCH50.gz > data/DAY.NASDAQ_ITCH50     (about 13 GB, delete afterwards)
#   scripts/panel_day.sh data/DAY.NASDAQ_ITCH50 [out-dir]
# Cuts the panel extract, then replay checks, fidelity with the displayed-queue audit,
# late-insert timing and queue tracking. Everything lands in out-dir (default out/panel/<day>).
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build}
IN=$1
NAME=$(basename "$IN")
OUT=${2:-out/panel/$NAME}
SYMS=$(grep -v '^#' scripts/panel_symbols.txt | tr -d ' \n')
mkdir -p "$OUT"
echo "== extract panel"
"$BUILD"/nfe_extract "$IN" "$OUT/panel.itch" "$SYMS"
echo "== replay"
"$BUILD"/nfe_replay "$OUT/panel.itch" --out "$OUT/replay" --strict | head -3
echo "== fidelity (with audit)"
"$BUILD"/nfe_fidelity "$OUT/panel.itch" --out "$OUT/fidelity" | head -20
echo "== late inserts"
"$BUILD"/nfe_lateadds "$OUT/panel.itch" --out "$OUT/late"
echo "== queue tracking"
"$BUILD"/nfe_queue "$OUT/panel.itch" --out "$OUT/queue" --day "$NAME" --symbols "$SYMS"
