"""Phase 4: what is queue position worth, in fill probability and adverse selection?

Reads one or more nfe_queue output directories (one per day), and reports by
decile of shares ahead at arrival (decile 1 = front of the queue):
  * fill probability within 1 s, 10 s and 60 s (any fill), unconditional and
    conditional on the order not being cancelled or replaced first;
  * markout after each fill at 100 ms, 1 s and 10 s: the move of Nasdaq's own
    mid against the order, in basis points (positive = adverse);
  * net capture: half-spread earned at the fill minus the markout.
Intervals are 95% percentile bootstraps that resample whole symbol-days.

Hypotheses (written before running, see docs/writeup.md):
  H1 fill probability falls monotonically with shares ahead;
  H2 back-of-queue fills have worse markouts;
  H3 both effects are larger in tick-constrained symbols.

    python3 analysis/queue_study.py out/day1/queue [out/day2/queue ...] --out results/queue
"""
import argparse
import json
import os

import numpy as np
import pandas as pd

from style import SERIES, TEXT2, line_with_band, setup

NS = 1_000_000_000
CLOSE = 16 * 3600 * NS
HORIZONS_FILL = [1, 10, 60]
HORIZONS_MARK = [0.1, 1, 10]
B = 1000


def load(dirs):
    orders, fills, mids = [], [], {}
    for d in dirs:
        o = pd.read_csv(os.path.join(d, "orders.csv"))
        day = o["day"].iloc[0] if len(o) else os.path.basename(d)
        f = pd.read_csv(os.path.join(d, "fills.csv"))
        f["day"] = day
        m = pd.read_csv(os.path.join(d, "mids.csv"))
        m = m[(m.bid > 0) & (m.ask > 0)]
        for loc, g in m.groupby("locate"):
            mids[(day, loc)] = (g.ts_ns.to_numpy(), ((g.bid + g.ask) / 2).to_numpy())
        orders.append(o)
        fills.append(f)
    return pd.concat(orders, ignore_index=True), pd.concat(fills, ignore_index=True), mids


def prepare(o):
    o = o[o.ts_ns <= CLOSE - 60 * NS].copy()  # every horizon fully observed before the close
    o["cluster"] = o.day.astype(str) + ":" + o.symbol
    # Orders that set a new best price have nobody ahead by construction; they
    # are group 0. Deciles 1-10 rank the orders that joined an existing level.
    join = o.improved == 0
    pct = o[join].groupby("cluster")["ahead"].rank(pct=True, method="first")
    o["decile"] = 0
    o.loc[join, "decile"] = np.clip(np.ceil(pct * 10).astype(int), 1, 10)
    tier = o.groupby("cluster").spread_ticks.median().eq(1).rename("tick_constrained")
    o = o.join(tier, on="cluster")
    wait = np.where(o.first_fill_ts > 0, o.first_fill_ts - o.ts_ns, np.inf)
    left = np.where(o.exit.isin(["D", "U"]), o.exit_ts - o.ts_ns, np.inf)
    for T in HORIZONS_FILL:
        filled = wait <= T * NS
        o[f"fill_{T}s"] = filled.astype(float)
        o[f"alive_{T}s"] = (filled | (left > T * NS)).astype(float)  # not cancelled before T
    return o


def markouts(f, o, mids):
    f = f.merge(o[["ref", "day", "decile", "cluster", "tick_constrained"]], on=["ref", "day"], how="inner")
    sign = np.where(f.side == "B", 1.0, -1.0)
    mid0 = np.full(len(f), np.nan)
    mh = {h: np.full(len(f), np.nan) for h in HORIZONS_MARK}
    for (day, loc), idx in f.groupby(["day", "locate"]).groups.items():
        if (day, loc) not in mids:
            continue
        ts, mid = mids[(day, loc)]
        t = f.loc[idx, "ts_ns"].to_numpy()
        i0 = np.searchsorted(ts, t, side="left") - 1  # last quote strictly before the fill
        ok = i0 >= 0
        mid0[np.asarray(idx)[ok]] = mid[i0[ok]]
        for h in HORIZONS_MARK:
            ih = np.searchsorted(ts, t + int(h * NS), side="right") - 1
            mh[h][np.asarray(idx)[ok]] = mid[ih[ok]]
    f["half_spread_bps"] = sign * (mid0 - f.price) / mid0 * 1e4
    for h in HORIZONS_MARK:
        f[f"markout_{h}s"] = sign * (mid0 - mh[h]) / mid0 * 1e4
        f[f"net_{h}s"] = f.half_spread_bps - f[f"markout_{h}s"]
    return f.dropna(subset=["half_spread_bps"])


def cluster_sums(df, value_cols, weight):
    """Per cluster and decile: weighted sums and weights, for fast bootstraps."""
    g = df.assign(_w=df[weight] if weight else 1.0)
    agg = {c: (c, lambda s, w=None: 0) for c in []}
    rows = []
    for (cl, dec), x in g.groupby(["cluster", "decile"]):
        r = {"cluster": cl, "decile": dec, "w": x._w.sum()}
        for c in value_cols:
            r[c] = (x[c] * x._w).sum()
        rows.append(r)
    return pd.DataFrame(rows)


def bootstrap(sums, value_cols, denom=None, rng=None):
    """Point estimate and 95% CI per decile for ratio-of-sums metrics."""
    clusters = sums.cluster.unique()
    cube = {c: sums.pivot(index="cluster", columns="decile", values=c).reindex(columns=range(0, 11)).fillna(0)
            for c in value_cols + ["w"] + ([denom] if denom else [])}
    base = {c: cube[c].to_numpy() for c in cube}
    draws = rng.integers(0, len(clusters), size=(B, len(clusters)))
    out = {}
    for c in value_cols:
        d = base[denom] if denom else base["w"]
        point = base[c].sum(0) / np.maximum(d.sum(0), 1e-12)
        boots = np.stack([base[c][ix].sum(0) / np.maximum(d[ix].sum(0), 1e-12) for ix in draws])
        out[c] = {"point": point, "lo": np.nanpercentile(boots, 2.5, axis=0),
                  "hi": np.nanpercentile(boots, 97.5, axis=0), "boots": boots}
    return out


def spearman_rows(boots):
    ranks = np.arange(1, 11)
    rho = []
    for row in boots[:, 1:]:
        ok = ~np.isnan(row)
        if ok.sum() < 3:
            continue
        r = pd.Series(row[ok]).rank().to_numpy()
        rho.append(np.corrcoef(ranks[ok], r)[0, 1])
    return np.array(rho)


def ci(a):
    a = a[~np.isnan(a)]
    return [float(np.mean(a)), float(np.percentile(a, 2.5)), float(np.percentile(a, 97.5))] if len(a) else None


def analyse(o, f, rng):
    fill_cols = [f"fill_{T}s" for T in HORIZONS_FILL]
    fs = cluster_sums(o, fill_cols + [f"alive_{T}s" for T in HORIZONS_FILL], None)
    fill = bootstrap(fs, fill_cols, rng=rng)
    cond = {}
    for T in HORIZONS_FILL:
        cond[f"fill_{T}s"] = bootstrap(fs, [f"fill_{T}s"], denom=f"alive_{T}s", rng=rng)[f"fill_{T}s"]
    mcols = [f"markout_{h}s" for h in HORIZONS_MARK] + [f"net_{h}s" for h in HORIZONS_MARK] + ["half_spread_bps"]
    ms = cluster_sums(f, mcols, "qty")
    mark = bootstrap(ms, mcols, rng=rng) if len(ms) else {}
    return fill, cond, mark


def summarise(fill, cond, mark, n_orders, n_fills, n_clusters):
    s = {"orders": int(n_orders), "fills": int(n_fills), "symbol_days": int(n_clusters), "deciles": {}}
    for k, v in list(fill.items()) + [("cond_" + k, v) for k, v in cond.items()] + list(mark.items()):
        s["deciles"][k] = {"point": np.round(v["point"], 6).tolist(), "lo": np.round(v["lo"], 6).tolist(),
                           "hi": np.round(v["hi"], 6).tolist()}
    b = fill["fill_10s"]["boots"]
    s["H1_spearman_rho_fill10s"] = ci(spearman_rows(b))
    s["H1_front_over_back_fill10s"] = ci(b[:, 1] / np.where(b[:, 10] > 0, b[:, 10], np.nan))
    if mark:
        m = mark["markout_1s"]["boots"]
        s["H2_back_minus_front_markout1s_bps"] = ci(m[:, 10] - m[:, 1])
    return s


def chart_fill(res, path, title):
    import matplotlib.pyplot as plt
    setup()
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    x = np.arange(0, 11)
    for i, T in enumerate(HORIZONS_FILL):
        v = res[f"fill_{T}s"]
        line_with_band(ax, x, v["point"], v["lo"], v["hi"], SERIES[i], f"within {T} s")
    ax.set_xticks(x, ["new\nbest"] + [str(i) for i in range(1, 11)])
    ax.set_xlabel("Decile of shares ahead at arrival (1 = front; 'new best' set a new price)")
    ax.set_ylabel("Probability of any fill")
    ax.set_ylim(0, 1)
    ax.set_title(title + "\n", loc="left")
    ax.text(0, 1.02, "Shaded: 95% bootstrap interval, resampling symbol-days", transform=ax.transAxes,
            color=TEXT2, fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def chart_markout(mark, path, title):
    import matplotlib.pyplot as plt
    setup()
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    x = np.arange(0, 11)
    for i, h in enumerate(HORIZONS_MARK):
        v = mark[f"markout_{h}s"]
        line_with_band(ax, x, v["point"], v["lo"], v["hi"], SERIES[i], f"{int(h * 1000)} ms" if h < 1 else f"{h:g} s")
    ax.axhline(0, color=TEXT2, linewidth=0.8)
    ax.set_xticks(x, ["new\nbest"] + [str(i) for i in range(1, 11)])
    ax.set_xlabel("Decile of shares ahead at arrival (1 = front; 'new best' set a new price)")
    ax.set_ylabel("Markout, bps (positive = adverse)")
    ax.set_title(title + "\n", loc="left")
    ax.text(0, 1.02, "Shaded: 95% bootstrap interval, resampling symbol-days", transform=ax.transAxes,
            color=TEXT2, fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--out", default="results/queue")
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rng = np.random.default_rng(a.seed)

    o, f, mids = load(a.dirs)
    o = prepare(o)
    f = markouts(f, o, mids)
    report = {}
    for name, sel in [("all", slice(None)), ("tick_constrained", True), ("wider_spread", False)]:
        oo = o if name == "all" else o[o.tick_constrained == sel]
        ff = f if name == "all" else f[f.tick_constrained == sel]
        if oo.cluster.nunique() < 2:
            continue
        fill, cond, mark = analyse(oo, ff, rng)
        report[name] = summarise(fill, cond, mark, len(oo), len(ff), oo.cluster.nunique())
        chart_fill(fill, os.path.join(a.out, f"fill_prob_{name}.png"),
                   f"Fill probability by queue position ({name.replace('_', ' ')})")
        if mark:
            chart_markout(mark, os.path.join(a.out, f"markout_{name}.png"),
                          f"Markout after a fill, by queue position ({name.replace('_', ' ')})")
    if "tick_constrained" in report and "wider_spread" in report:
        t, w = report["tick_constrained"], report["wider_spread"]
        report["H3"] = {"front_over_back_fill10s": {"tick_constrained": t["H1_front_over_back_fill10s"],
                                                     "wider_spread": w["H1_front_over_back_fill10s"]},
                        "back_minus_front_markout1s_bps": {"tick_constrained": t.get("H2_back_minus_front_markout1s_bps"),
                                                           "wider_spread": w.get("H2_back_minus_front_markout1s_bps")}}
    json.dump(report, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
    a_ = report["all"]
    print(json.dumps({k: a_[k] for k in a_ if k.startswith("H") or k in ("orders", "fills", "symbol_days")}, indent=1))
    if "H3" in report:
        print(json.dumps(report["H3"], indent=1))


if __name__ == "__main__":
    main()
