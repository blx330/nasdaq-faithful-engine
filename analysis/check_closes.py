"""Independent check: closing cross prices against official closing prices.

The official closes come from outside the feed. For the synthetic day the
simulator writes them; for a real day, put a CSV with columns symbol,close
from an external source (an exchange or vendor close file) next to the data.

    python3 analysis/check_closes.py out/day/crosses.csv closes.csv [--json out.json]
"""
import argparse
import csv
import json


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("crosses")
    ap.add_argument("closes")
    ap.add_argument("--json")
    a = ap.parse_args()
    ours = {}
    for r in csv.DictReader(open(a.crosses)):
        if r["cross_type"] == "C":
            ours[r["symbol"]] = float(r["price"])
    official = {r["symbol"]: float(r["close"]) for r in csv.DictReader(open(a.closes))}
    both = sorted(set(ours) & set(official))
    mism = [(s, ours[s], official[s]) for s in both if abs(ours[s] - official[s]) > 1e-6]
    res = {"symbols_compared": len(both), "matches": len(both) - len(mism),
           "missing_cross": sorted(set(official) - set(ours)), "mismatches": mism[:50]}
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    print(json.dumps(res, indent=1))
    raise SystemExit(1 if mism else 0)


if __name__ == "__main__":
    main()
