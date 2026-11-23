"""Queue study on real panel days: what is queue position worth?

Same measures as queue_study.py (fill probability by decile of shares ahead,
markouts, symbol-day bootstrap), built for the real 25-symbol panel, where one
day has about 5M tracked orders. Each day is reduced to per-symbol-day sums
before the next is loaded, so it runs in under 4 GB. Added for real data:

  * orders arriving before 09:31:00 or after 15:59:00 are dropped (the opening
    release places orders by entry order, so shares ahead at arrival is not
    well defined in the first seconds of the day);
  * orders with any fill at a price different from their displayed price are
    tagged `better_price`; results are reported with and without them, and
    their share of fills is reported;
  * a linear probability model of a fill within 10 s on log shares ahead with
    controls (imbalance toward the order, spread, order size, hour) and
    symbol-day fixed effects, standard errors clustered by symbol-day;
  * results by liquidity tier (labels from scripts/panel_symbols.txt).

    python3 analysis/queue_panel.py out/panel/*/queue --out results/queue-real
"""
import argparse
import json
import os
import re
import zlib

import numpy as np
import pandas as pd

from queue_study import (HORIZONS_FILL, HORIZONS_MARK, NS, bootstrap, chart_markout, ci, cluster_sums,
                         markouts, spearman_rows)

OPEN = (9 * 3600 + 31 * 60) * NS
LAST = (15 * 3600 + 59 * 60) * NS
REG_SAMPLE = 0.2  # share of joiners kept for the regression (memory)

O_COLS = {"day": "category", "symbol": "category", "ref": "int64", "locate": "int32", "ts_ns": "int64",
          "side": "category", "price": "int64", "qty": "int64", "ahead": "int64", "spread_ticks": "int32",
          "imbalance": "float32", "improved": "int8", "first_fill_ts": "int64", "exit_ts": "int64", "exit": "category"}


def tiers(path):
    t = {}
    for line in open(path):
        m = re.match(r"#\s*(tier \d|bridge)[^:]*:\s*(\S+)", line)
        if m:
            for s in m.group(2).split(","):
                t[s] = m.group(1)
    return t


def load_day(d, rng):
    parts, day = [], None
    for ch in pd.read_csv(os.path.join(d, "orders.csv"), usecols=list(O_COLS), dtype=O_COLS, chunksize=1_000_000):
        day = day or str(ch["day"].iloc[0])
        parts.append(ch[(ch.ts_ns >= OPEN) & (ch.ts_ns <= LAST)].drop(columns=["day"]))
    o = pd.concat(parts, ignore_index=True)
    del parts
    # categoricals, not strings: one day has millions of rows
    o["symbol"] = o["symbol"].astype(str).astype("category")
    o["cluster"] = o["symbol"].cat.rename_categories(lambda s: day + ":" + s)
    o["day"] = pd.Categorical([day] * len(o))
    f = pd.read_csv(os.path.join(d, "fills.csv"))
    f["day"] = day
    f = f.merge(o[["ref", "price"]].rename(columns={"price": "shown"}), on="ref", how="inner")
    better = set(f.loc[f.price != f.shown, "ref"])
    o["better_price"] = o.ref.isin(better)
    join = o.improved == 0
    pct = o[join].groupby("cluster", observed=True)["ahead"].rank(pct=True, method="first")
    o["decile"] = 0
    o.loc[join, "decile"] = np.clip(np.ceil(pct * 10).astype(int), 1, 10)
    tc = o.groupby("cluster", observed=True).spread_ticks.median().eq(1).rename("tick_constrained")
    o = o.join(tc, on="cluster")
    wait = np.where(o.first_fill_ts > 0, o.first_fill_ts - o.ts_ns, np.inf)
    left = np.where(o.exit.isin(["D", "U"]), o.exit_ts - o.ts_ns, np.inf)
    for T in HORIZONS_FILL:
        filled = wait <= T * NS
        o[f"fill_{T}s"] = filled.astype(np.float32)
        o[f"alive_{T}s"] = (filled | (left > T * NS)).astype(np.float32)
    m = pd.read_csv(os.path.join(d, "mids.csv"))
    m = m[(m.bid > 0) & (m.ask > 0)]
    mids = {(day, loc): (g.ts_ns.to_numpy(), ((g.bid + g.ask) / 2).to_numpy()) for loc, g in m.groupby("locate")}
    del m
    fm = markouts(f.drop(columns=["shown"]), o, mids)
    fm = fm.merge(o[["ref", "better_price"]], on="ref", how="left")
    # regression sample: joiners only
    j = o[join]
    j = j[rng.random(len(j)) < REG_SAMPLE]
    sign = np.where(j.side == "B", 1.0, -1.0)
    reg = pd.DataFrame({
        "cluster": j.cluster.astype(str).to_numpy(), "fill_10s": j.fill_10s.to_numpy(np.float64),
        "log_ahead": np.log1p(j.ahead.to_numpy(np.float64)), "imb_toward": sign * j.imbalance.to_numpy(np.float64),
        "spread_ticks": np.minimum(j.spread_ticks.to_numpy(np.float64), 10), "log_qty": np.log(j.qty.to_numpy(np.float64).clip(1)),
        "hour": (j.ts_ns.to_numpy() // (3600 * NS)).astype(int), "better_price": j.better_price.to_numpy()})
    stats = {"day": day, "orders": int(len(o)), "joiners": int(join.sum()), "new_best": int((~join).sum()),
             "tracked_fills": int(len(f)), "fills_better_price": int((f.price != f.shown).sum()),
             "fill_shares": int(f.qty.sum()), "fill_shares_better_price": int(f.qty[f.price != f.shown].sum()),
             "orders_better_price": int(o.better_price.sum())}
    fill_cols = [f"fill_{T}s" for T in HORIZONS_FILL] + [f"alive_{T}s" for T in HORIZONS_FILL]
    mcols = [f"markout_{h}s" for h in HORIZONS_MARK] + [f"net_{h}s" for h in HORIZONS_MARK] + ["half_spread_bps"]
    meta = o.groupby("cluster", observed=True).agg(symbol=("symbol", "first"), tick_constrained=("tick_constrained", "first"),
                                    orders=("ref", "size"), median_ahead=("ahead", "median")).reset_index()
    meta["cluster"] = meta["cluster"].astype(str)
    meta["symbol"] = meta["symbol"].astype(str)
    out = {}
    for name, oo, ff in [("all", o, fm), ("ex_better_price", o[~o.better_price], fm[fm.better_price == False])]:  # noqa: E712
        out[name] = (cluster_sums(oo, fill_cols, None), cluster_sums(ff, mcols, "qty") if len(ff) else pd.DataFrame())
    return stats, meta, out, reg


def analyse(fs, ms, rng):
    fill_cols = [f"fill_{T}s" for T in HORIZONS_FILL]
    fill = bootstrap(fs, fill_cols, rng=rng)
    cond = {f"fill_{T}s": bootstrap(fs, [f"fill_{T}s"], denom=f"alive_{T}s", rng=rng)[f"fill_{T}s"] for T in HORIZONS_FILL}
    mcols = [f"markout_{h}s" for h in HORIZONS_MARK] + [f"net_{h}s" for h in HORIZONS_MARK] + ["half_spread_bps"]
    # bootstrap() draws clusters by position, so fills and markouts need the same cluster index
    ms = ms[ms.cluster.isin(fs.cluster.unique())]
    mark = bootstrap(ms, mcols, rng=rng) if len(ms) and ms.cluster.nunique() > 1 else {}
    return fill, cond, mark


def summarise(fill, cond, mark, n_clusters):
    s = {"symbol_days": int(n_clusters), "deciles": {}}
    for k, v in list(fill.items()) + [("cond_" + k, v) for k, v in cond.items()] + list(mark.items()):
        s["deciles"][k] = {"point": np.round(v["point"], 6).tolist(), "lo": np.round(v["lo"], 6).tolist(),
                           "hi": np.round(v["hi"], 6).tolist()}
    b = fill["fill_10s"]["boots"]
    p = fill["fill_10s"]["point"]
    s["front_over_back_fill10s"] = {"point": float(p[1] / p[10]) if p[10] > 0 else None,
                                    "boot": ci(b[:, 1] / np.where(b[:, 10] > 0, b[:, 10], np.nan))}
    s["spearman_rho_fill10s"] = ci(spearman_rows(b))
    s["newbest_minus_front_fill10s"] = {"point": float(p[0] - p[1]), "boot": ci(b[:, 0] - b[:, 1])}
    if mark:
        m = mark["markout_1s"]["boots"]
        mp = mark["markout_1s"]["point"]
        s["back_minus_front_markout1s_bps"] = {"point": float(mp[10] - mp[1]), "boot": ci(m[:, 10] - m[:, 1])}
        # joiners pooled (deciles 1-10) are not a ratio of sums here; compare with the middle of the queue too
        s["newbest_minus_front_markout1s_bps"] = {"point": float(mp[0] - mp[1]), "boot": ci(m[:, 0] - m[:, 1])}
        s["newbest_minus_decile5_markout1s_bps"] = {"point": float(mp[0] - mp[5]), "boot": ci(m[:, 0] - m[:, 5])}
    return s


def chart_real(fill, cond, path, n_days):
    """One figure for real data: 10 s fill probability, all orders and orders not cancelled first.

    The shared chart in queue_study.py fixes the y axis at 0 to 1, which flattens real data, where
    most orders are cancelled within seconds and unconditional fill rates are under 10%.
    """
    import matplotlib.pyplot as plt
    from style import SERIES, TEXT2, setup
    setup()
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.2))
    x = np.arange(0, 11)
    for ax, v, title, color in [(axes[0], fill["fill_10s"], "All tracked orders", SERIES[0]),
                                (axes[1], cond["fill_10s"], "Orders not cancelled or replaced first", SERIES[1])]:
        ax.fill_between(x, v["lo"], v["hi"], color=color, alpha=0.15, linewidth=0)
        ax.plot(x, v["point"], color=color, marker="o", markersize=6)
        ax.set_xticks(x, ["new\nbest"] + [str(i) for i in range(1, 11)])
        ax.set_ylim(0, max(v["hi"]) * 1.15)
        ax.set_title(title, loc="left", fontsize=10)
        ax.set_xlabel("Decile of shares ahead at arrival (1 = front)")
    axes[0].set_ylabel("Probability of a fill within 10 s")
    fig.suptitle("Fill probability by queue position, Nasdaq ITCH, 25 symbols", x=0.01, ha="left", fontweight="bold")
    fig.text(0.01, 0.005, f"{n_days} symbol-days. Shaded: 95% bootstrap interval, resampling symbol-days. Note the two y scales.",
             color=TEXT2, fontsize=8)
    fig.tight_layout(rect=(0, 0.03, 1, 0.95))
    fig.savefig(path, dpi=150)
    plt.close(fig)


def regression(reg, path):
    import statsmodels.api as sm
    out = {}
    lines = []
    for name, r in [("all joiners", reg), ("excluding better-price orders", reg[~reg.better_price])]:
        X = pd.concat([r[["log_ahead", "imb_toward", "spread_ticks", "log_qty"]],
                       pd.get_dummies(r.hour, prefix="h", drop_first=True, dtype=float)], axis=1)
        y = r.fill_10s
        g = r.cluster
        Xd = X - X.groupby(g).transform("mean")  # symbol-day fixed effects
        yd = y - y.groupby(g).transform("mean")
        fit = sm.OLS(yd, Xd).fit(cov_type="cluster", cov_kwds={"groups": pd.factorize(g)[0]})
        raw = sm.OLS(y - y.groupby(g).transform("mean"),
                     (r[["log_ahead"]] - r[["log_ahead"]].groupby(g).transform("mean"))).fit(
            cov_type="cluster", cov_kwds={"groups": pd.factorize(g)[0]})
        keep = ["log_ahead", "imb_toward", "spread_ticks", "log_qty"]
        out[name] = {"n": int(len(r)), "symbol_days": int(g.nunique()), "mean_fill_10s": float(y.mean()),
                     "log_ahead_no_controls": [float(raw.params["log_ahead"]), float(raw.bse["log_ahead"])],
                     "coef": {k: [float(fit.params[k]), float(fit.bse[k])] for k in keep}}
        lines.append(f"== {name}: n={len(r)}, symbol-days={g.nunique()}, mean fill_10s={y.mean():.4f}")
        lines.append(f"   log_ahead alone (symbol-day FE): {raw.params['log_ahead']:+.5f} (se {raw.bse['log_ahead']:.5f})")
        for k in keep:
            lines.append(f"   {k:13s} {fit.params[k]:+.5f} (se {fit.bse[k]:.5f}, t {fit.tvalues[k]:+.1f})")
    open(path, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--out", default="results/queue-real")
    ap.add_argument("--tiers", default="scripts/panel_symbols.txt")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--cache", help="directory for per-day reductions, so a run can be resumed day by day")
    ap.add_argument("--prepare-only", action="store_true", help="reduce the given days into --cache and stop")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rng = np.random.default_rng(a.seed)
    tier_of = tiers(a.tiers)

    stats, metas, regs = [], [], []
    sums = {"all": ([], []), "ex_better_price": ([], [])}
    for d in a.dirs:
        cp = os.path.join(a.cache, os.path.basename(os.path.dirname(os.path.abspath(d))) + ".pkl") if a.cache else None
        if cp and os.path.exists(cp):
            st, meta, out, reg = pd.read_pickle(cp)
        else:
            st, meta, out, reg = load_day(d, np.random.default_rng([a.seed, zlib.crc32(os.path.abspath(d).split(os.sep)[-2].encode())]))
            if cp:
                os.makedirs(a.cache, exist_ok=True)
                pd.to_pickle((st, meta, out, reg), cp)
        print(json.dumps(st), flush=True)
        stats.append(st)
        metas.append(meta)
        regs.append(reg)
        for k, (fs, ms) in out.items():
            sums[k][0].append(fs)
            sums[k][1].append(ms)
    if a.prepare_only:
        return
    meta = pd.concat(metas, ignore_index=True)
    meta["tier"] = meta.symbol.map(tier_of).fillna("?")
    meta.to_csv(os.path.join(a.out, "symbol_days.csv"), index=False)
    tot = {k: sum(s[k] for s in stats) for k in stats[0] if k != "day"}
    report = {"days": stats, "total": tot,
              "better_price_share_of_fills": tot["fills_better_price"] / tot["tracked_fills"],
              "better_price_share_of_fill_shares": tot["fill_shares_better_price"] / tot["fill_shares"],
              "window": "09:31:00 to 15:59:00", "groups": {}}

    def run(name, fs, ms, charts=False):
        if fs.cluster.nunique() < 2:
            return
        fill, cond, mark = analyse(fs, ms, rng)
        report["groups"][name] = summarise(fill, cond, mark, fs.cluster.nunique())
        if charts:
            chart_real(fill, cond, os.path.join(a.out, f"fill_prob_{name}.png"), fs.cluster.nunique())
            if mark:
                chart_markout(mark, os.path.join(a.out, f"markout_{name}.png"), f"Markout after a fill, by queue position ({name.replace('_', ' ')})")

    for k, (fl, ml) in sums.items():
        fs = pd.concat(fl, ignore_index=True)
        ms = pd.concat([m for m in ml if len(m)], ignore_index=True)
        run(k, fs, ms, charts=True)
        if k == "all":
            for t in sorted(meta.tier.unique()):
                cl = set(meta.cluster[meta.tier == t])
                run("tier:" + t, fs[fs.cluster.isin(cl)], ms[ms.cluster.isin(cl)])
            for lab, flag in [("tick_constrained", True), ("wider_spread", False)]:
                cl = set(meta.cluster[meta.tick_constrained == flag])
                run(lab, fs[fs.cluster.isin(cl)], ms[ms.cluster.isin(cl)])
    report["regression"] = regression(pd.concat(regs, ignore_index=True), os.path.join(a.out, "regression.txt"))
    json.dump(report, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
    for k, v in report["groups"].items():
        print(k, json.dumps({x: v[x] for x in v if x != "deciles"}))
    print("better-price share of fills:", report["better_price_share_of_fills"], "of shares:", report["better_price_share_of_fill_shares"])


if __name__ == "__main__":
    main()
