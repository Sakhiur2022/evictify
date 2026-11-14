#!/usr/bin/env python3
"""
Eviction-timeline plot: WHY does a policy lose hot blocks?

Builds the real bio.c on the host for LRU / Clock / Clock-NR, runs one workload
with trace=N, parses the trace and draws, per policy:
  * every miss as a dot at (request #, victim buffer index)
  * red X  = a HOT-SET block was evicted
  * strip at the top: hot-set requests (green = hit, red = miss)

    python3 trace_plot.py --src ../xv6-lru                   # defaults: NBUF=60 hotscan scan=120
    python3 trace_plot.py --src ../xv6-lru --nbuf 30 --workload 4 --scan 120 --hot 8 --out plots

Needs gcc + matplotlib.  --src is the directory holding bio.c and bcsim/.
"""
import argparse, os, re, shutil, subprocess, tempfile
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ============================ EDIT ME ==================================
POLICIES = {"LRU": "-DBCACHE_LRU", "Clock": "", "Clock-NR": "-DBCACHE_CLOCK_NOREF_ON_LOAD"}
COLORS   = {"LRU": "#7aa6ff", "Clock": "#ff8fab", "Clock-NR": "#5fd3a5"}
BG, INK, GRID = "#fffaf3", "#3b3548", "#eadfcf"
# =======================================================================

LINE = re.compile(r"^T(\d+) blk=(\d+)(\(H\))? (HIT|MISS)\s+lk=(\d+)(.*)$")

def build_and_run(src, flags, nbuf, args, tmp, tag):
    exe = os.path.join(tmp, f"sim_{tag}")
    subprocess.run(["gcc", "-w", "-O1", *flags.split(), f"-DNBUF={nbuf}", "-DBCACHE_LOOKUP_HASH",
                    "-I" + tmp, "-I" + os.path.join(src, "bcsim"), "-I" + src,
                    "-o", exe, os.path.join(tmp, "bio.c"),
                    os.path.join(src, "bcsim", "stubs.c"), os.path.join(src, "bcsim", "main.c")], check=True)
    return subprocess.run([exe, *map(str, args)], capture_output=True, text=True, check=True).stdout

def parse(text):
    rows = []
    for l in text.splitlines():
        m = LINE.match(l)
        if not m: continue
        n, blk, hot, kind, lk, rest = m.groups()
        v = re.search(r"victim=buf(\d+)", rest)
        rows.append(dict(n=int(n), blk=int(blk), hot=bool(hot), hit=kind == "HIT",
                         victim=int(v.group(1)) if v else None, hot_evict="HOT-EVICT" in rest))
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="../xv6-lru"); ap.add_argument("--out", default="plots")
    ap.add_argument("--nbuf", type=int, default=60)
    ap.add_argument("--workload", type=int, default=2, help="1 seq 2 hotscan 3 wset 4 pollute 5 mixed")
    ap.add_argument("--scan", type=int, default=120); ap.add_argument("--hot", type=int, default=4)
    ap.add_argument("--interval", type=int, default=40); ap.add_argument("--passes", type=int, default=3)
    ap.add_argument("--rounds", type=int, default=2); ap.add_argument("--trace", type=int, default=600)
    a = ap.parse_args()
    args = [a.workload, a.scan, a.hot, a.interval, a.passes, a.rounds, a.trace]
    tmp = tempfile.mkdtemp(); shutil.copy(os.path.join(a.src, "bio.c"), tmp)
    data = {}
    try:
        for name, fl in POLICIES.items():
            data[name] = parse(build_and_run(a.src, fl, a.nbuf, args, tmp, name.replace("-", "")))
    finally:
        shutil.rmtree(tmp)

    plt.rcParams.update({"figure.facecolor": BG, "axes.facecolor": BG, "savefig.facecolor": BG,
                         "axes.edgecolor": GRID, "axes.labelcolor": INK, "text.color": INK,
                         "xtick.color": INK, "ytick.color": INK, "axes.grid": True, "grid.color": GRID,
                         "grid.linestyle": "--", "axes.spines.top": False, "axes.spines.right": False})
    fig, axes = plt.subplots(len(data), 1, figsize=(13, 3.1 * len(data)), sharex=True, sharey=True)
    for ax, (name, rows) in zip(axes, data.items()):
        miss = [r for r in rows if not r["hit"]]
        ax.scatter([r["n"] for r in miss], [r["victim"] for r in miss], s=16, color=COLORS[name],
                   edgecolor="white", linewidth=0.5, zorder=3, label="miss: victim buffer")
        ev = [r for r in miss if r["hot_evict"]]
        ax.scatter([r["n"] for r in ev], [r["victim"] for r in ev], marker="X", s=110, color="#d6456b",
                   edgecolor="white", linewidth=1, zorder=4, label=f"hot block evicted ({len(ev)})")
        for r in rows:                                   # hot-request strip along the top
            if r["hot"]:
                ax.plot([r["n"]] * 2, [a.nbuf + 1, a.nbuf + 4], lw=1.6,
                        color="#2f9e73" if r["hit"] else "#d6456b", zorder=2)
        ax.set_ylim(-3, a.nbuf + 6); ax.set_ylabel("victim buffer index")
        ax.set_title(f"{name}:  {len(miss)} misses, {len(ev)} hot evictions in first {len(rows)} requests",
                     loc="left", fontsize=11, fontweight="bold")
        ax.legend(loc="upper left", fontsize=8, frameon=False, ncol=1)
    axes[-1].set_xlabel("request number   (top strip: hot-set requests, green = hit, red = miss)")
    fig.suptitle(f"Eviction timeline  -  NBUF={a.nbuf}, workload {a.workload}, scan={a.scan}, hot={a.hot}",
                 fontsize=15, fontweight="bold", color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    os.makedirs(a.out, exist_ok=True)
    p = os.path.join(a.out, "fig10_eviction_timeline.png")
    fig.savefig(p, dpi=160, bbox_inches="tight"); print("wrote", p)

if __name__ == "__main__":
    main()
