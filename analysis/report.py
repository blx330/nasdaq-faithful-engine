"""Collect every pillar's numbers into results/REPORT.md and draw the headline charts.

    python3 analysis/report.py --results results --days out/day1 out/day2 ...
"""
import argparse
import glob
import json
import os

import numpy as np
import pandas as pd

from style import NEUTRAL, SERIES, TEXT2, setup

CLASSES = ["match", "match_hidden", "remainder_repriced", "nondisplay_price", "non_displayed", "hidden_only", "grouping",
           "cross", "unexplained"]
LABELS = {"match": "Exact match", "match_hidden": "Exact (hidden also hit)", "nondisplay_price": "Non-display price (C)",
          "non_displayed": "Non-displayed liquidity (P)", "hidden_only": "Hidden only, nothing displayed",
          "grouping": "Grouping ambiguity", "remainder_repriced": "Remainder repriced (Price to Comply)", "cross": "Opening/closing/halt cross", "unexplained": "Unexplained"}


def load(path):
    return json.load(open(path)) if os.path.exists(path) else None


def fidelity_tables(days):
    tot = {c: 0 for c in CLASSES}
    per = []
    for d in days:
        f = load(os.path.join(d, "fidelity.json"))
        if not f:
            continue
        for c in CLASSES:
            tot[c] += f["by_class"].get(c, 0)
        p = pd.read_csv(os.path.join(d, "per_symbol.csv"))
        p["day"] = os.path.basename(d)
        per.append(p)
    per = pd.concat(per, ignore_index=True) if per else pd.DataFrame()
    return tot, per


def rates(t):
    cont = sum(t[c] for c in CLASSES if c != "cross")
    displayed = cont - t["hidden_only"]
    no_hidden = t["match"] + t["grouping"] + t["unexplained"] + t["nondisplay_price"] + t["remainder_repriced"]
    return {
        "continuous_events": cont,
        "exact_rate": (t["match"] + t["match_hidden"]) / displayed if displayed else None,
        "exact_rate_no_hidden": t["match"] / no_hidden if no_hidden else None,
        "unexplained_share": t["unexplained"] / cont if cont else None,
        "grouping_share": t["grouping"] / cont if cont else None,
    }


def tier_table(per):
    if per.empty:
        return pd.DataFrame()
    p = per.copy()
    if "remainder_repriced" not in p:
        p["remainder_repriced"] = 0
    p["continuous"] = p.messages - p.cross
    p["displayed"] = p.continuous - p.hidden_only
    p["tier"] = np.where(p.one_tick_events > 0.5 * p.displayed, "tick-constrained", "wider spread")
    act = p.groupby("symbol").continuous.sum()
    terc = pd.qcut(act.rank(method="first"), 3, labels=["low activity", "mid activity", "high activity"]) if len(act) >= 3 else None
    if terc is not None:
        p["activity"] = p.symbol.map(terc)
    rows = []
    for col in ["tier"] + (["activity"] if terc is not None else []):
        for k, g in p.groupby(col, observed=True):
            disp = g.displayed.sum()
            rows.append({"group": k, "symbol_days": len(g), "events": int(g.continuous.sum()),
                         "exact_rate": (g.match.sum() + g.match_hidden.sum()) / disp if disp else np.nan,
                         "unexplained": int(g.unexplained.sum())})
    return pd.DataFrame(rows)


def chart_taxonomy(t, path):
    import matplotlib.pyplot as plt
    setup()
    order = [c for c in CLASSES if t[c] > 0 or c == "unexplained"]
    vals = [t[c] for c in order]
    total = sum(vals)
    fig, ax = plt.subplots(figsize=(7.5, 0.45 * len(order) + 1.2))
    y = np.arange(len(order))[::-1]
    colors = [SERIES[0] if c in ("match", "match_hidden") else NEUTRAL if c != "unexplained" else SERIES[7] for c in order]
    ax.barh(y, vals, color=colors, height=0.6)
    for yi, v in zip(y, vals):
        ax.annotate(f"{v:,}  ({v / total:.1%})", (v, yi), xytext=(4, 0), textcoords="offset points", va="center", color=TEXT2, fontsize=9)
    ax.set_yticks(y, [LABELS[c] for c in order])
    ax.set_xlim(0, max(vals) * 1.3)
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("Matching events")
    ax.set_title("Every execution event, classified")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def chart_time(bench, path):
    import matplotlib.pyplot as plt
    setup()
    designs = [d for d in ("baseline", "fast") if d in bench]
    parse_ns = 1e3 / bench["parse_only_mmsg_s"][0]
    comps = {}
    for d in designs:
        total = 1e3 / bench[d]["replay_mmsg_s"][0]
        book = 1e3 / bench[d]["book_only_mops_s"][0] * bench["book_ops"] / bench["messages"]
        comps[d] = [parse_ns, book, max(total - parse_ns - book, 0)]
    names = ["Parsing", "Book operations", "Everything else"]
    fig, ax = plt.subplots(figsize=(7.5, 3.2))
    for i, d in enumerate(designs):
        left = 0
        for j, (n, v) in enumerate(zip(names, comps[d])):
            ax.barh(i, v, left=left, color=SERIES[j], height=0.55, edgecolor="#fcfcfb", linewidth=2,
                    label=n if i == 0 else None)
            left += v
        ax.annotate(f"{left:.0f} ns/msg", (left, i), xytext=(6, 0), textcoords="offset points", va="center", color=TEXT2)
    ax.set_yticks(range(len(designs)), designs)
    ax.invert_yaxis()
    ax.set_xlabel("Nanoseconds per ITCH message (mean of runs)")
    ax.set_title("Where the time goes")
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.3), ncol=3, fontsize=9)
    ax.set_xlim(0, max(sum(v) for v in comps.values()) * 1.25)
    ax.grid(axis="y", visible=False)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def pct(x):
    return "n/a" if x is None or (isinstance(x, float) and np.isnan(x)) else f"{100 * x:.2f}%"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--days", nargs="+", required=True)
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    R = a.results
    os.makedirs(R, exist_ok=True)

    lines = [f"# Results{(' - ' + a.label) if a.label else ''}", ""]
    # Pillar 0: reconstruction
    lines += ["## Reconstruction", "", "| Day | Messages | Unknown refs | Overfills | Crossed (continuous) | Crossed (other states) | Closing crosses matched |",
              "| --- | --- | --- | --- | --- | --- | --- |"]
    for d in a.days:
        r = load(os.path.join(d, "replay.json"))
        c = load(os.path.join(d, "closes.json"))
        if r:
            cc = f"{c['matches']} of {c['symbols_compared']}" if c else "no official closes supplied"
            lines.append(f"| {os.path.basename(d)} | {r['messages']:,} | {r['unknown_ref']} | {r['overfill']} | "
                         f"{r['crossed_continuous']} | {r['crossed_other']:,} | {cc} |")
    # Pillar 1: fidelity
    tot, per = fidelity_tables(a.days)
    rt = rates(tot)
    chart_taxonomy(tot, os.path.join(R, "residual_taxonomy.png"))
    lines += ["", "## Fidelity", "",
              f"**Exact-match rate {pct(rt['exact_rate'])}** of displayed-book matching events in continuous trading "
              f"({rt['continuous_events']:,} events); {pct(rt['exact_rate_no_hidden'])} for events with no hidden "
              f"involvement. Unexplained: {tot['unexplained']} ({pct(rt['unexplained_share'])}). Grouping ambiguity: "
              f"{pct(rt['grouping_share'])}.", "", "![Residual taxonomy](residual_taxonomy.png)", "",
              "| Class | Events |", "| --- | --- |"] + [f"| {LABELS[c]} | {tot[c]:,} |" for c in CLASSES]
    tt = tier_table(per)
    if not tt.empty:
        lines += ["", "| Group | Symbol-days | Events | Exact-match rate | Unexplained |", "| --- | --- | --- | --- | --- |"]
        lines += [f"| {r.group} | {r.symbol_days} | {r.events:,} | {pct(r.exact_rate)} | {r.unexplained} |" for r in tt.itertuples()]
    scores = [load(p) for p in sorted(glob.glob(os.path.join(R, "grouping_score_*.json")))]
    scores = [s for s in scores if s]
    if scores:
        n = sum(s["truth_aggressors"] for s in scores)
        one = sum(s["one_to_one"] for s in scores)
        lines += ["", f"Grouping heuristic against ground truth (synthetic days only): {one:,} of {n:,} true aggressors "
                  f"recovered one-to-one ({pct(one / n)}); every timestamp with several aggressors was flagged "
                  f"({sum(s['multi_flagged'] for s in scores)} of {sum(s['multi_aggressor_timestamps'] for s in scores)}); "
                  f"posted remainders recovered {pct(min(s['posted_recovered'] for s in scores))} (worst day)."]
    # Pillar 2: test power
    mm = os.path.join(R, "mutant_matrix.md")
    if os.path.exists(mm):
        lines += ["", "## Test power", "", open(mm).read()]
    # Pillar 3: finding
    q = load(os.path.join(R, "queue", "summary.json"))
    if q:
        al = q["all"]
        lines += ["", "## Queue position", "",
                  f"{al['orders']:,} orders at the touch, {al['fills']:,} fills, {al['symbol_days']} symbol-days. "
                  "Point estimate [95% CI], bootstrapped by symbol-day.", "",
                  f"- H1, fill probability falls with shares ahead: Spearman rho over deciles (10 s) "
                  f"{al['H1_spearman_rho_fill10s'][0]:.2f} [{al['H1_spearman_rho_fill10s'][1]:.2f}, {al['H1_spearman_rho_fill10s'][2]:.2f}]; "
                  f"front decile fills {al['H1_front_over_back_fill10s'][0]:.2f}x as often as the back "
                  f"[{al['H1_front_over_back_fill10s'][1]:.2f}, {al['H1_front_over_back_fill10s'][2]:.2f}]."]
        if al.get("H2_back_minus_front_markout1s_bps"):
            h2 = al["H2_back_minus_front_markout1s_bps"]
            lines.append(f"- H2, back-of-queue fills are more adversely selected: back minus front markout at 1 s "
                         f"{h2[0]:+.2f} bps [{h2[1]:+.2f}, {h2[2]:+.2f}].")
        if "H3" in q:
            h3 = q["H3"]
            f3 = h3["front_over_back_fill10s"]
            lines.append(f"- H3, tick-constrained vs wider spread: front/back fill ratio "
                         f"{f3['tick_constrained'][0]:.2f} vs {f3['wider_spread'][0]:.2f}; back-minus-front markout "
                         f"{h3['back_minus_front_markout1s_bps']['tick_constrained'][0]:+.2f} vs "
                         f"{h3['back_minus_front_markout1s_bps']['wider_spread'][0]:+.2f} bps.")
        lines += ["", "![Fill probability](queue/fill_prob_all.png)", "", "![Markout](queue/markout_all.png)"]
    # Pillar 4: performance
    b = load(os.path.join(R, "bench.json"))
    if b:
        chart_time(b, os.path.join(R, "time_attribution.png"))
        lines += ["", "## Performance", "", f"{b['messages']:,} messages, {b['runs']} runs after a discarded warm-up, mean ± sd.", "",
                  "| Design | Replay (M msg/s) | Book only (M op/s) | p50 | p99 | p99.9 |", "| --- | --- | --- | --- | --- | --- |"]
        for d in ("baseline", "fast"):
            if d in b:
                x = b[d]
                lines.append(f"| {d} | {x['replay_mmsg_s'][0]:.2f} ± {x['replay_mmsg_s'][1]:.2f} | "
                             f"{x['book_only_mops_s'][0]:.2f} ± {x['book_only_mops_s'][1]:.2f} | {x['latency_ns']['p50']:.0f} ns | "
                             f"{x['latency_ns']['p99']:.0f} ns | {x['latency_ns']['p99_9']:.0f} ns |")
        lines.append(f"\nParse only: {b['parse_only_mmsg_s'][0]:.1f} M msg/s. Timer overhead (p50): "
                     f"{b['baseline']['latency_ns']['timer_overhead_p50']:.0f} ns, included in the latencies above.")
        ct = load(os.path.join(R, "cpptrader.json"))
        if ct:
            best = b.get("fast", b["baseline"])["replay_mmsg_s"][0]
            lines.append(f"\nCppTrader market manager on the same file and machine: {ct['mmsg_s']:.2f} M msg/s "
                         f"(median of {ct['runs']}). Ratio, this engine / CppTrader: **{best / ct['mmsg_s']:.2f}x**. "
                         "Work per message differs; see docs/perf-log.md.")
        lines += ["", "![Time attribution](time_attribution.png)"]
    open(os.path.join(R, "REPORT.md"), "w").write("\n".join(lines) + "\n")
    headline = {"exact_rate": rt["exact_rate"], "exact_rate_no_hidden": rt["exact_rate_no_hidden"],
                "unexplained": tot["unexplained"]}
    ms = load(os.path.join(R, "mutants_summary.json"))
    if ms:
        headline["mutants"] = f"{ms['caught']} of {ms['total']}"
    json.dump(headline, open(os.path.join(R, "headline.json"), "w"), indent=1)
    print(json.dumps(headline))


if __name__ == "__main__":
    main()
