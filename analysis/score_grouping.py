"""Score the aggressor-grouping heuristic against the simulator's ground truth.

Only possible on synthetic data, where every incoming order is known. For
each (symbol, timestamp) in continuous trading it compares the true
aggressors with the events the fidelity pipeline inferred.

    python3 analysis/score_grouping.py out/synth/events.csv data/truth.jsonl [--json out.json]
"""
import argparse
import csv
import json
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("events")
    ap.add_argument("truth")
    ap.add_argument("--json")
    a = ap.parse_args()

    truth = defaultdict(list)
    for line in open(a.truth):
        t = json.loads(line)
        truth[(t["loc"], t["ts"])].append(t)
    inferred = defaultdict(list)
    for r in csv.DictReader(open(a.events)):
        if r["class"] == "cross":
            continue
        inferred[(int(r["locate"]), int(r["ts_ns"]))].append(r)

    n_truth = sum(len(v) for v in truth.values())
    one_to_one = merged = split = missing = 0
    side_ok = qty_ok = limit_ok = posted_n = posted_ok = 0
    multi_keys = flagged_multi = 0
    for key, ts in truth.items():
        inf = inferred.get(key, [])
        if len(ts) > 1:
            multi_keys += 1
            if len(inf) != len(ts) or any(r["class"] == "grouping" or r["same_ts"] == "1" for r in inf):
                flagged_multi += 1
        if not inf:
            missing += len(ts)
        elif len(inf) < len(ts):
            merged += len(ts) - len(inf)
        elif len(inf) > len(ts):
            split += len(inf) - len(ts)
        if len(ts) == 1 and len(inf) == 1:
            t, r = ts[0], inf[0]
            one_to_one += 1
            side_ok += r["side"] in (t["side"], "?")
            qty_ok += int(r["qty"]) == t["qty"] - (t["qty"] - sum(f[1] for f in t["fills"]) - t["posted"])
            lim = int(r["limit"])
            limit_ok += r["side"] == "?" or (lim <= t["limit"] if t["side"] == "B" else lim >= t["limit"])
            if t["posted"]:
                posted_n += 1
                posted_ok += int(r["qty"]) == t["qty"] and lim == t["limit"]

    res = {
        "truth_aggressors": n_truth,
        "inferred_events": sum(len(v) for v in inferred.values()),
        "one_to_one": one_to_one,
        "merged_aggressors": merged,
        "split_events": split,
        "missing": missing,
        "grouping_error_rate": round((n_truth - one_to_one) / n_truth, 6) if n_truth else None,
        "side_correct": round(side_ok / one_to_one, 6) if one_to_one else None,
        "qty_correct": round(qty_ok / one_to_one, 6) if one_to_one else None,
        "limit_consistent": round(limit_ok / one_to_one, 6) if one_to_one else None,
        "posted_remainders": posted_n,
        "posted_recovered": round(posted_ok / posted_n, 6) if posted_n else None,
        "multi_aggressor_timestamps": multi_keys,
        "multi_flagged": flagged_multi,
    }
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
