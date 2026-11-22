#!/usr/bin/env python3
"""Scale-up analysis: late inserts, tiers and release delay on a symbol-sharded full day.

  analysis/scale_up.py --shards DIR/s0 DIR/s1 ... --tiers tiers.json --out results/scale-up

Each shard directory holds the output of nfe_fidelity (per_symbol.csv, fidelity.json)
and nfe_lateadds (late.csv, symbols.csv, crosses.csv, states.csv) for a disjoint set of
symbols. Predictions being scored are in docs/predictions-scale-up.md.
"""
import argparse, json, os
import numpy as np
import pandas as pd

NS = 1e9
OPEN_WALL = 34200.0  # 09:30:00 in seconds after midnight

ap = argparse.ArgumentParser()
ap.add_argument("--shards", nargs="+", required=True)
ap.add_argument("--tiers", required=True)
ap.add_argument("--out", required=True)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)

def cat(name):
    return pd.concat([pd.read_csv(os.path.join(d, name)).assign(shard=i) for i, d in enumerate(a.shards)], ignore_index=True)

tiers = json.load(open(a.tiers))
rank = tiers["rank_by_msgs"]; msgs = tiers["msgs"]
def tier_of(sym):
    r = rank.get(sym)
    if r is None: return "none"
    if r <= 50: return "T1"
    if r <= 500: return "T2"
    if r <= 2000: return "T3"
    return "T4" if msgs[sym] >= 2000 else "T5"

sy = cat("symbols.csv")
sy["tier"] = sy.symbol.map(tier_of)
sy["msgs"] = sy.symbol.map(msgs)
late = cat("late.csv").merge(sy[["shard", "locate", "symbol", "tier", "market_category", "etp"]], on=["shard", "locate"])
cr = cat("crosses.csv")
st = cat("states.csv")

# open cross: first opening cross (type O) per symbol
op = cr[cr.type == "O"].sort_values("ts_ns").groupby(["shard", "locate"]).ts_ns.first().rename("open_ns").reset_index()
sy = sy.merge(op, on=["shard", "locate"], how="left")
late = late.merge(op, on=["shard", "locate"], how="left")
late["t"] = late.ts_ns / NS
late["off_open"] = (late.ts_ns - late.open_ns) / NS
late["off_wall"] = late.t - OPEN_WALL
halts = st[st.state != "T"].groupby(["shard", "locate"]).size().rename("n_nonT_states").reset_index()
sy = sy.merge(halts, on=["shard", "locate"], how="left").fillna({"n_nonT_states": 0})

res = {}
n = len(late)
res["late_inserts_total"] = int(n)
res["symbols_with_late"] = int((sy.late > 0).sum())
res["symbols_total"] = int(len(sy))

# ---- P1: timing of late inserts
w = late.dropna(subset=["off_open"])
res["late_with_open_cross"] = int(len(w))
res["late_without_open_cross"] = int(n - len(w))
for lim in (5, 15, 40, 45, 60, 120):
    res[f"share_within_{lim}s_of_own_open"] = float(((w.off_open >= 0) & (w.off_open <= lim)).mean())
res["share_before_own_open"] = float((w.off_open < 0).mean())
res["wall_0930_to_093045"] = float(((late.off_wall >= 0) & (late.off_wall <= 45)).mean())
res["wall_before_0930"] = float((late.off_wall < 0).mean())
res["late_after_093045_wall"] = int((late.off_wall > 45).sum())
res["late_after_own_open_plus_45s"] = int((w.off_open > 45).sum())
# symbols whose own open is not at 09:30:00
sy["open_wall"] = sy.open_ns / NS - OPEN_WALL
res["symbols_with_open_cross"] = int(sy.open_ns.notna().sum())
res["symbols_open_after_5s_wall"] = int((sy.open_wall > 5).sum())
res["symbols_open_after_60s_wall"] = int((sy.open_wall > 60).sum())
out_late = w[w.off_open > 45]
res["late_beyond_45s_by_symbol_top"] = out_late.groupby("symbol").size().sort_values(ascending=False).head(15).to_dict()
res["late_beyond_45s_symbols"] = int(out_late.symbol.nunique())
# of those, symbols with non-T trading states (halts, pauses, quote-only)
bs = out_late.groupby(["shard", "locate"]).size().rename("n").reset_index().merge(sy[["shard", "locate", "n_nonT_states", "symbol"]], on=["shard", "locate"])
res["late_beyond_45s_in_symbols_with_nonT_state"] = int(bs[bs.n_nonT_states > 0].n.sum())
# hour histogram of late inserts beyond 45 s
res["late_beyond_45s_hour_hist"] = (out_late.t // 3600).astype(int).value_counts().sort_index().to_dict()

# ---- P3: tiers from fidelity per_symbol.csv
ps = cat("per_symbol.csv").rename(columns={"messages": "events"})
ps["tier"] = ps.symbol.map(tier_of)
cls = ["match", "match_hidden", "non_displayed", "nondisplay_price", "cross", "grouping", "remainder_repriced", "hidden_only", "unexplained"]
g = ps.groupby("tier")[["events"] + cls].sum()
g["continuous"] = g.events - g.cross
g["displayed"] = g.continuous - g.hidden_only
g["exact_displayed"] = (g.match + g.match_hidden) / g.displayed
g["hidden_share_cont"] = (g.match_hidden + g.hidden_only + g.non_displayed) / g.continuous
g["hidden_only_share"] = g.hidden_only / g.continuous
g["nondisp_price_share"] = g.nondisplay_price / g.continuous
g["unexplained_rate"] = g.unexplained / g.continuous
lt = sy.groupby("tier")[["adds", "late"]].sum()
g = g.join(lt)
g["late_per_1000_adds"] = 1000 * g.late / g.adds
g["symbols"] = ps.groupby("tier").symbol.nunique()
g.to_csv(os.path.join(a.out, "by_tier.csv"))
res["by_tier"] = json.loads(g.to_json(orient="index"))

# ---- P4: release delay per symbol
MIN_LATE = 5
d = w[(w.off_open >= 0) & (w.off_open <= 120)].groupby(["shard", "locate"]).off_open.agg(["count", "median", "min", "max"]).reset_index()
d = d.merge(sy[["shard", "locate", "symbol", "tier", "market_category", "etp", "open_wall"]], on=["shard", "locate"])
d = d[d["count"] >= MIN_LATE]
d.to_csv(os.path.join(a.out, "release_delay_by_symbol.csv"), index=False)
res["release_symbols_with_min_late"] = int(len(d))
bins = [0, 0.5, 1, 1.5, 2, 3, 5, 8, 10, 11, 12, 15, 20, 30, 60, 120]
res["release_delay_median_hist"] = {f"{bins[i]}-{bins[i+1]}": int(c) for i, c in enumerate(np.histogram(d["median"], bins=bins)[0])}
res["release_delay_by_market_category"] = {k: {"n": int(len(v)), "median_of_medians": float(v["median"].median()), "p10": float(v["median"].quantile(.1)), "p90": float(v["median"].quantile(.9))} for k, v in d.groupby("market_category")}
res["release_delay_by_tier"] = {k: {"n": int(len(v)), "median_of_medians": float(v["median"].median()), "p10": float(v["median"].quantile(.1)), "p90": float(v["median"].quantile(.9))} for k, v in d.groupby("tier")}
res["release_delay_by_etp"] = {k: {"n": int(len(v)), "median_of_medians": float(v["median"].median())} for k, v in d.groupby("etp")}
# late insert totals by category
res["late_by_market_category"] = sy.groupby("market_category")[["adds", "late"]].sum().assign(per_1000=lambda x: 1000 * x.late / x.adds).round(3).to_dict(orient="index")
# variance explained (eta squared) of the per-symbol median delay by category vs tier
def eta2(col):
    m = d["median"].mean(); tot = ((d["median"] - m) ** 2).sum()
    return float(sum(len(v) * (v["median"].mean() - m) ** 2 for _, v in d.groupby(col)) / tot) if tot else float("nan")
res["eta2_market_category"] = eta2("market_category"); res["eta2_tier"] = eta2("tier"); res["eta2_etp"] = eta2("etp")

# per-symbol late counts (out-of-order Add counts)
sy.sort_values("late", ascending=False).to_csv(os.path.join(a.out, "late_by_symbol.csv"), index=False)
res["late_top10"] = sy.sort_values("late", ascending=False).head(10)[["symbol", "tier", "market_category", "adds", "late"]].to_dict(orient="records")
json.dump(res, open(os.path.join(a.out, "scale_up.json"), "w"), indent=1, default=str)
print(json.dumps(res, indent=1, default=str))
