#!/usr/bin/env bash
# External baseline: CppTrader's market manager on the same file and machine.
#
#   scripts/cpptrader_baseline.sh <itch-file> [json-out] [runs]
#
# Builds https://github.com/chronoxor/CppTrader (needs `pip install gil`)
# into third_party/ and runs cpptrader-performance-market_manager, which
# parses ITCH and maintains full order books: the closest match to our
# replay. Its "optimized" variants preallocate 300M order slots and need
# far more memory; run them by hand if the machine allows.
set -euo pipefail
FILE=$(realpath "$1")
OUT=${2:-results/cpptrader.json}
RUNS=${3:-5}
DIR=third_party/CppTrader
CORE=${CORE:-$(( $(nproc) - 1 ))}

if [ ! -x "$DIR/temp/cpptrader-performance-market_manager" ]; then
  mkdir -p third_party
  [ -d "$DIR" ] || git clone --depth 1 https://github.com/chronoxor/CppTrader.git "$DIR"
  (cd "$DIR" && gil update && mkdir -p temp && cd temp &&
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCPPTRADER_MODULE=OFF >/dev/null &&
    make -j"$(nproc)" cpptrader-performance-market_manager >/dev/null)
fi
PIN=""
command -v taskset >/dev/null 2>&1 && PIN="taskset -c $CORE"
rates=()
for i in $(seq "$RUNS"); do
  r=$($PIN "$DIR/temp/cpptrader-performance-market_manager" < "$FILE" | awk '/ITCH message throughput/ {print $4}')
  rates+=("$r")
done
median=$(printf '%s\n' "${rates[@]}" | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')
all=$(IFS=,; echo "${rates[*]}")
python3 -c "import json; json.dump({'mmsg_s': $median/1e6, 'runs': $RUNS, 'all': [x/1e6 for x in [$all]]}, open('$OUT','w'))"
echo "CppTrader market manager: $(python3 -c "print(round($median/1e6,2))") M msg/s (median of $RUNS)"
