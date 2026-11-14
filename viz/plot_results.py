#!/usr/bin/env python3
"""
Cute plots for the xv6 buffer-cache benchmark (LRU vs Clock vs Clock-NR).

    python3 plot_results.py                      # reads results.csv, writes ./plots/*.png
    python3 plot_results.py --csv path/to.csv --out my_plots --lookup list

Everything you are likely to tweak lives in the EDIT ME block below.
Needs: matplotlib, pandas, numpy.   (pip install matplotlib pandas numpy)
results.csv is produced by `bash compare_bcache.sh` (bcsim/run_matrix.py).
"""
import argparse, os
import numpy as np, pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

# ============================ EDIT ME ==================================
LOOKUP   = "hash"                 # "hash" isolates the replacement policy; "list" = as-built xv6
NBUFS    = [30, 60, 90]
POLICIES = ["LRU", "CLOCK", "CLOCK-NR"]
LABELS   = {"LRU": "LRU (xv6 default)", "CLOCK": "Clock", "CLOCK-NR": "Clock-NR"}
COLORS   = {"LRU": "#7aa6ff", "CLOCK": "#ff8fab", "CLOCK-NR": "#5fd3a5"}   # blue / pink / mint
BG, INK, GRID = "#fffaf3", "#3b3548", "#eadfcf"
PASSES, ROUNDS = 3, 5             # rows in the CSV with other values are ignored
# (label, workload id, scan, hot, interval)   ids: 1 seq 2 hotscan 3 wset 4 pollute 5 mixed
CONFIGS = [
    ("seq\nscan 240",       1, 240,  4, 40),
    ("hotscan\nscan 120",   2, 120,  4, 40),
    ("hotscan\nscan 240",   2, 240,  4, 40),
    ("wset\nhot 48",        3,   1, 48, 40),
    ("pollute\nscan 40",    4,  40,  8, 40),
    ("pollute\nscan 120",   4, 120,  8, 40),
    ("mixed\nscan 120",     5, 120,  8, 40),
    ("mixed\nscan 240",     5, 240,  8, 40),
]
SPOTLIGHT = ("mixed\nscan 120", 5, 120, 8, 40)   # used by the cost-breakdown figure
DPI = 160
# =======================================================================

from matplotlib.colors import LinearSegmentedColormap
WINLOSS = LinearSegmentedColormap.from_list("winloss", ["#2f9e73", "#9be3c3", "#f6efe2", "#ffb3c4", "#d6456b"])  # green=better

plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": BG, "savefig.facecolor": BG,
    "axes.edgecolor": GRID, "axes.labelcolor": INK, "text.color": INK,
    "xtick.color": INK, "ytick.color": INK, "axes.grid": True, "grid.color": GRID,
    "grid.linestyle": "--", "grid.linewidth": 0.8, "axes.axisbelow": True,
    "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 10, "axes.titleweight": "bold", "axes.titlesize": 12,
    "legend.frameon": False,
})

def load(path, lookup):
    df = pd.read_csv(path)
    df.columns = [c.strip() for c in df.columns]
    df = df[(df.lookup == lookup) & (df.passes == PASSES) & (df.rounds == ROUNDS)
            | (df.workload.isin([0, 6]))]
    df = df[df.lookup == lookup].copy()
    df["hit_pct"] = 100 * df.hits / df.requests
    df["hot_pct"] = 100 * df.hot_hits / df.hot_req.replace(0, np.nan)
    df["ret_pct"] = 100 * df.ws_res / df.ws_probes.replace(0, np.nan)
    df["mgmt_us"] = df.lookup_us + df.repl_us
    df["iohit_us"] = df.io_us + df.hit_us
    df["lookup_per_req"] = df.lookup_insp / df.requests
    return df

def pick(df, cfg, nbuf, policy):
    _, w, scan, hot, itv = cfg
    r = df[(df.workload == w) & (df.scan == scan) & (df.hot == hot) &
           (df.interval == itv) & (df.nbuf == nbuf) & (df.policy == policy)]
    return r.iloc[0] if len(r) else None

def title(fig, text, sub=None):
    h = fig.get_figheight()
    fig.text(0.5, 1 - 0.10 / h, text, ha="center", va="top", fontsize=16, fontweight="bold", color=INK)
    if sub:
        fig.text(0.5, 1 - 0.50 / h, sub, ha="center", va="top", fontsize=10, color="#7a7388")

def top(fig):
    """Top edge (figure fraction) left free for the title block."""
    return 1 - 0.95 / fig.get_figheight()

def save(fig, outdir, name):
    p = os.path.join(outdir, name)
    fig.savefig(p, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print("wrote", p)

def rounded_bars(ax, xs, hs, w, color, label=None):
    """Bars with softly rounded look (white outline + alpha)."""
    return ax.bar(xs, hs, w, color=color, edgecolor="white", linewidth=1.6, label=label, zorder=3)

# ---------------------------------------------------------------- fig 1
def fig_misses(df, out):
    fig, axes = plt.subplots(1, len(NBUFS), figsize=(5.2 * len(NBUFS), 5), sharey=True)
    w = 0.26
    for ax, nb in zip(np.atleast_1d(axes), NBUFS):
        x = np.arange(len(CONFIGS))
        for i, p in enumerate(POLICIES):
            vals = [pick(df, c, nb, p).misses for c in CONFIGS]
            bars = rounded_bars(ax, x + (i - 1) * w, vals, w, COLORS[p], LABELS[p])
            for b, v in zip(bars, vals):
                ax.text(b.get_x() + b.get_width() / 2, v, f"{int(v)}", ha="center",
                        va="bottom", fontsize=6.5, rotation=90, color=INK)
        ax.set_xticks(x); ax.set_xticklabels([c[0] for c in CONFIGS], fontsize=7.5)
        ax.set_title(f"NBUF = {nb}")
        ax.set_ylim(0, ax.get_ylim()[1] * 1.12)
    np.atleast_1d(axes)[0].set_ylabel("cache misses (lower is better)")
    np.atleast_1d(axes)[0].legend(loc="upper left")
    title(fig, "Who misses less?", f"same request sequence for every bar | lookup = {LOOKUP}")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig1_misses.png")

# ---------------------------------------------------------------- fig 2
def fig_heat(df, out, col, name, ttl, cmap, fmt="{:.0f}", vmin=None, vmax=None):
    cols = [(nb, p) for nb in NBUFS for p in POLICIES]
    M = np.full((len(CONFIGS), len(cols)), np.nan)
    for i, c in enumerate(CONFIGS):
        for j, (nb, p) in enumerate(cols):
            r = pick(df, c, nb, p)
            if r is not None: M[i, j] = r[col]
    keep = [i for i in range(len(CONFIGS)) if not np.all(np.isnan(M[i]))]   # drop rows with no data (e.g. seq has no hot set)
    M = M[keep]; cfgs = [CONFIGS[i] for i in keep]
    fig, ax = plt.subplots(figsize=(1.0 * len(cols) + 3, 0.62 * len(cfgs) + 2.2))
    im = ax.imshow(M, cmap=cmap, aspect="auto", vmin=vmin, vmax=vmax)
    ax.grid(False)
    for i in range(M.shape[0]):
        for j in range(M.shape[1]):
            if not np.isnan(M[i, j]):
                dark = im.norm(M[i, j]) > 0.55          # white text on dark cells
                ax.text(j, i, fmt.format(M[i, j]), ha="center", va="center", fontsize=8.5,
                        color="white" if dark else INK, fontweight="bold")
    ax.set_xticks(range(len(cols)))
    ax.set_xticklabels([{"LRU": "LRU", "CLOCK": "Clock", "CLOCK-NR": "NR"}[p] for _, p in cols], fontsize=8)
    ax.set_yticks(range(len(cfgs))); ax.set_yticklabels([c[0].replace("\n", " ") for c in cfgs], fontsize=8.5)
    for k, nb in enumerate(NBUFS):                      # group labels + separators
        ax.text(k * 3 + 1, -0.9, f"NBUF {nb}", ha="center", fontsize=10, fontweight="bold")
        if k: ax.axvline(k * 3 - 0.5, color=BG, lw=5)
    for sp in ax.spines.values(): sp.set_visible(False)
    fig.colorbar(im, ax=ax, fraction=0.025, pad=0.02)
    title(fig, ttl, f"lookup = {LOOKUP}")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, name)

# ---------------------------------------------------------------- fig 3
def fig_delta(df, out):
    fig, axes = plt.subplots(1, 2, figsize=(11, 0.6 * len(CONFIGS) + 2.6))
    for ax, p in zip(axes, ["CLOCK", "CLOCK-NR"]):
        M = np.array([[pick(df, c, nb, p).misses - pick(df, c, nb, "LRU").misses
                       for nb in NBUFS] for c in CONFIGS], float)
        lim = max(1, np.abs(M).max())
        im = ax.imshow(M, cmap=WINLOSS, vmin=-lim, vmax=lim, aspect="auto")
        ax.grid(False)
        for i in range(M.shape[0]):
            for j in range(M.shape[1]):
                v = int(M[i, j]); s = "tie" if v == 0 else f"{v:+d}"
                ax.text(j, i, s, ha="center", va="center", fontsize=10, fontweight="bold",
                        color="white" if abs(v) > 0.6 * lim else INK)
        ax.set_xticks(range(len(NBUFS))); ax.set_xticklabels([f"NBUF {n}" for n in NBUFS])
        ax.set_yticks(range(len(CONFIGS)))
        ax.set_yticklabels([c[0].replace("\n", " ") for c in CONFIGS] if p == "CLOCK" else [])
        ax.set_title(f"{LABELS[p]} - LRU (misses)")
        for s in ax.spines.values(): s.set_visible(False)
    title(fig, "Win / tie / loss map", "green = Clock variant misses LESS than LRU | red = MORE | tie = equal")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig3_delta_misses.png")

# ---------------------------------------------------------------- fig 4
def fig_cost(df, out):
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(13, 5.2), gridspec_kw={"width_ratios": [1.1, 1]})
    x = np.arange(len(NBUFS)); w = 0.26
    parts = [("iohit_us", "I/O + hits", "#b9c7ff"), ("lookup_us", "lookup", "#ffd89b"), ("repl_us", "replacement", "#ff9bb3")]
    for i, p in enumerate(POLICIES):
        bottom = np.zeros(len(NBUFS))
        for col, lab, colr in parts:
            vals = np.array([pick(df, SPOTLIGHT, nb, p)[col] for nb in NBUFS]) / 1000
            a1.bar(x + (i - 1) * w, vals, w, bottom=bottom, color=colr, edgecolor="white", linewidth=1.4,
                   label=lab if i == 0 else None, zorder=3)
            bottom += vals
        for xi, tot in zip(x + (i - 1) * w, bottom):
            a1.text(xi, tot, {"LRU": "LRU", "CLOCK": "Clk", "CLOCK-NR": "NR"}[p], ha="center", va="bottom", fontsize=7.5)
    a1.set_xticks(x); a1.set_xticklabels([f"NBUF {n}" for n in NBUFS])
    a1.set_ylabel("modeled cost (ms)"); a1.set_ylim(0, a1.get_ylim()[1] * 1.18); a1.legend(loc="upper center", ncol=3)
    a1.set_title("Where does the time go?  (miss I/O dominates)")
    # management only
    for i, p in enumerate(POLICIES):
        look = np.array([pick(df, SPOTLIGHT, nb, p).lookup_us for nb in NBUFS]) / 1000
        repl = np.array([pick(df, SPOTLIGHT, nb, p).repl_us for nb in NBUFS]) / 1000
        a2.bar(x + (i - 1) * w, look, w, color="#ffd89b", edgecolor="white", linewidth=1.4, zorder=3)
        a2.bar(x + (i - 1) * w, repl, w, bottom=look, color=COLORS[p], edgecolor="white", linewidth=1.4,
               label=LABELS[p], zorder=3)
    a2.set_xticks(x); a2.set_xticklabels([f"NBUF {n}" for n in NBUFS])
    a2.set_ylabel("management cost (ms)"); a2.set_ylim(0, a2.get_ylim()[1] * 1.2); a2.legend(loc="upper left", ncol=3, fontsize=8)
    a2.set_title("Zoom: lookup (yellow) + replacement (policy colour)")
    title(fig, f"Cost breakdown - {SPOTLIGHT[0].replace(chr(10), ' ')}", f"lookup = {LOOKUP}; unit costs identical for all policies")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig4_cost_breakdown.png")

# ---------------------------------------------------------------- fig 5
def fig_retention(df, out):
    sel = [c for c in CONFIGS if c[1] in (2, 4, 5)]
    ncol = min(3, len(sel)); nrow = -(-len(sel) // ncol)
    fig, axes = plt.subplots(nrow, ncol, figsize=(4.2 * ncol, 3.6 * nrow + 0.4), sharey=True, squeeze=False)
    for ax in axes.flat[len(sel):]:
        ax.axis("off")
    for ax, c in zip(axes.flat, sel):
        for i, p in enumerate(POLICIES):
            ys = [pick(df, c, nb, p).ret_pct for nb in NBUFS]
            dodge = (i - 1) * 0.9          # nudge x so identical lines stay visible
            ax.plot(np.array(NBUFS) + dodge, ys, "-o", color=COLORS[p], lw=2.6, ms=8, mec="white", mew=1.5,
                    label=LABELS[p], zorder=3)
        ax.set_title(c[0].replace("\n", " "), fontsize=10); ax.set_xticks(NBUFS); ax.set_xlabel("NBUF")
        ax.set_ylim(-6, 108)
    for r in range(nrow):
        axes[r, 0].set_ylabel("hot blocks still cached (%)")
    axes[0, 0].legend(loc="lower right", fontsize=8)
    title(fig, "Working-set retention", "fraction of the hot set resident when each hot phase begins (x nudged slightly so overlapping lines show)")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig5_retention.png")

# ---------------------------------------------------------------- fig 6
def fig_repl_ops(df, out):
    nb = NBUFS[min(1, len(NBUFS) - 1)]
    fig, ax = plt.subplots(figsize=(11, 5))
    x = np.arange(len(CONFIGS)); w = 0.26
    for i, p in enumerate(POLICIES):
        vals = [pick(df, c, nb, p).vic_insp + (pick(df, c, nb, p).lru_ptrw if p == "LRU" else
                pick(df, c, nb, p).ref_clears + pick(df, c, nb, p).ref_set_hit +
                pick(df, c, nb, p).ref_set_load + pick(df, c, nb, p).hand_writes) for c in CONFIGS]
        rounded_bars(ax, x + (i - 1) * w, vals, w, COLORS[p], LABELS[p])
    ax.set_xticks(x); ax.set_xticklabels([c[0] for c in CONFIGS], fontsize=8)
    ax.set_ylabel("replacement operations (reads + writes)"); ax.legend()
    title(fig, f"Replacement bookkeeping at NBUF = {nb}",
          "LRU pays 6 pointer writes on every release; Clock pays ref-bit sets/clears + hand sweeps")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig6_replacement_ops.png")

# ---------------------------------------------------------------- fig 7 (needs both lookups)
def fig_lookup(csv, out):
    full = pd.read_csv(csv); full.columns = [c.strip() for c in full.columns]
    full = full[(full.passes == PASSES) & (full.rounds == ROUNDS) & full.workload.isin([1, 2, 3, 4, 5])]
    full["lpr"] = full.lookup_insp / full.requests
    nb = NBUFS[min(1, len(NBUFS) - 1)]
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.8), sharey=True)
    for ax, lk in zip(axes, ["list", "hash"]):
        d = full[full.lookup == lk]
        x = np.arange(len(CONFIGS)); w = 0.26
        for i, p in enumerate(POLICIES):
            vals = []
            for c in CONFIGS:
                r = d[(d.workload == c[1]) & (d.scan == c[2]) & (d.hot == c[3]) & (d.interval == c[4]) &
                      (d.nbuf == nb) & (d.policy == p)]
                vals.append(r.lpr.iloc[0] if len(r) else 0)
            rounded_bars(ax, x + (i - 1) * w, vals, w, COLORS[p], LABELS[p])
        ax.set_xticks(x); ax.set_xticklabels([c[0] for c in CONFIGS], fontsize=7)
        ax.set_title(f"{lk} lookup")
    axes[0].set_ylabel("entries inspected per request")
    axes[0].set_ylim(0, axes[0].get_ylim()[1] * 1.22); axes[0].legend(fontsize=8, ncol=3, loc="upper center")
    title(fig, f"Lookup cost depends on the data structure, not just the policy (NBUF = {nb})",
          "list: LRU keeps hot blocks near the head, Clock's list never reorders | hash: same for both")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig7_lookup_list_vs_hash.png")

# ---------------------------------------------------------------- fig 8
def fig_scoreboard(df, out):
    fig, ax = plt.subplots(figsize=(8.5, 3.4))
    for k, p in enumerate(["CLOCK", "CLOCK-NR"]):
        win = tie = loss = 0
        for c in CONFIGS:
            for nb in NBUFS:
                d = pick(df, c, nb, p).misses - pick(df, c, nb, "LRU").misses
                win += d < 0; loss += d > 0; tie += d == 0
        left = 0
        for n, colr, lab in [(win, "#5fd3a5", "fewer misses"), (tie, "#ffe08a", "tie"), (loss, "#ff8fab", "more misses")]:
            ax.barh(k, n, left=left, color=colr, edgecolor="white", linewidth=2, height=0.55, zorder=3,
                    label=lab if k == 0 else None)
            if n: ax.text(left + n / 2, k, str(n), ha="center", va="center", fontweight="bold")
            left += n
    ax.set_yticks([0, 1]); ax.set_yticklabels(["Clock", "Clock-NR"]); ax.invert_yaxis()
    ax.set_xlabel(f"configurations (of {len(CONFIGS) * len(NBUFS)}) vs LRU")
    ax.legend(ncol=3, loc="upper center", bbox_to_anchor=(0.5, 1.22))
    title(fig, "Scoreboard vs LRU (misses)"); fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig8_scoreboard.png")

# ---------------------------------------------------------------- fig 9
def fig_interval(df, out):
    fig, axes = plt.subplots(1, len(NBUFS), figsize=(4.2 * len(NBUFS), 4), sharey=True)
    for ax, nb in zip(np.atleast_1d(axes), NBUFS):
        sub = df[(df.workload == 2) & (df.scan == 120) & (df.hot == 4) & (df.nbuf == nb)]
        for p in POLICIES:
            s = sub[sub.policy == p].sort_values("interval")
            ax.plot(s.interval, s.hot_pct, "-o", color=COLORS[p], lw=2.6, ms=8, mec="white", mew=1.5,
                    label=LABELS[p], zorder=3)
        ax.set_title(f"NBUF = {nb}"); ax.set_xlabel("hot-set revisit interval (scan blocks)")
    np.atleast_1d(axes)[0].set_ylabel("hot-set hit rate (%)"); np.atleast_1d(axes)[0].legend(fontsize=8)
    title(fig, "How often is the hot set revisited?", "hotscan, scan 120: shorter reuse distance = easier to protect")
    fig.tight_layout(rect=(0, 0, 1, top(fig))); save(fig, out, "fig9_interval_sensitivity.png")

def main():
    global LOOKUP
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="results.csv"); ap.add_argument("--out", default="plots")
    ap.add_argument("--lookup", default=LOOKUP)
    a = ap.parse_args()
    LOOKUP = a.lookup
    os.makedirs(a.out, exist_ok=True)
    df = load(a.csv, LOOKUP)
    fig_misses(df, a.out)
    fig_heat(df, a.out, "hot_pct", "fig2_hot_hit_heatmap.png", "Hot-set hit rate (%)", "YlGnBu", "{:.0f}", 0, 100)
    fig_delta(df, a.out)
    fig_cost(df, a.out)
    fig_retention(df, a.out)
    fig_repl_ops(df, a.out)
    fig_lookup(a.csv, a.out)
    fig_scoreboard(df, a.out)
    fig_interval(df, a.out)

if __name__ == "__main__":
    main()
