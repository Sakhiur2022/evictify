#!/usr/bin/env python3
"""Compare an evolved policy file with the LRU / Clock / Clock-NR seeds on the TRAIN and HELDOUT suites.

    python3 compare_best.py openevolve_output/best/best_program.h [--out best_vs_baselines]

Everything is measured in the same framework (hash lookup, identical request sequences, cold start and unit
costs).  Writes <out>.csv (one row per policy x workload x cache size) and <out>.md (summary + per-cell table).
Nothing is estimated: every number comes from running bio.c on the host.
"""
import csv, math, os, sys, tempfile, shutil
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import harness, lint, suite

DEFINES = ["-DBCACHE_EVOLVED", "-DBCACHE_LOOKUP_HASH"]
SEEDS = [("LRU", "lru.h"), ("Clock", "clock.h"), ("Clock-NR", "clock_nr.h"), ("GCLOCK-8", "gclock8.h")]


def gm(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cand = os.path.abspath(sys.argv[1])
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else "best_vs_baselines"
    probs = lint.check(open(cand).read(), frame_reference=open(os.path.join(HERE, "seeds", "lru.h")).read())
    if probs:
        sys.exit("candidate fails lint:\n  " + "\n  ".join(probs))
    pols = [(n, os.path.join(HERE, "seeds", f)) for n, f in SEEDS] + [("EVOLVED", cand)]
    tmp = tempfile.mkdtemp(); rows = []
    try:
        for split, nbufs, cfgs in (("train", suite.TRAIN_NBUF, suite.TRAIN), ("heldout", suite.HELDOUT_NBUF, suite.HELDOUT)):
            for nb in nbufs:
                for pname, path in pols:
                    exe = harness.build(tmp, f"{split}_{pname}_{nb}".replace("-", "_"), nb, DEFINES, policy=path)
                    for wname, cfg in cfgs:
                        r = harness.run(exe, cfg)
                        rows.append(dict(split=split, policy=pname, nbuf=nb, workload=wname, requests=r["requests"], hits=r["hits"],
                                         misses=r["misses"], hot_hit_pct=(100 * r["hot_hits"] / r["hot_req"]) if r["hot_req"] else "",
                                         lookup_us=r["lookup_us"], repl_us=r["repl_us"], total_us=r["total_us"],
                                         pol_reads=r["pol_reads"], pol_writes=r["pol_writes"], mem_policy_bytes=r["mem_policy"]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    with open(out + ".csv", "w", newline="") as f:
        w = csv.DictWriter(f, list(rows[0])); w.writeheader(); w.writerows(rows)

    def idx(split, pol): return {(r["nbuf"], r["workload"]): r for r in rows if r["split"] == split and r["policy"] == pol}
    L = ["# Evolved policy vs. baselines", "",
         f"Candidate: `{os.path.basename(cand)}`. Hash lookup, identical request sequences, cold start and unit costs for all rows.",
         "Cost ratio = LRU total modeled cost / policy total modeled cost (above 1.00 means cheaper than LRU).", ""]
    for split in ("train", "heldout"):
        base = idx(split, "LRU")
        L += [f"## {split} suite", "", "| policy | cost ratio (GM) | miss ratio (GM) | worst cell | cells better / worse | policy state (bytes, mid cache size) | mgmt share |", "|---|--:|--:|--:|--:|--:|--:|"]
        for pname, _ in pols:
            p = idx(split, pname); keys = list(base)
            cr = [base[k]["total_us"] / p[k]["total_us"] for k in keys]
            mr = [p[k]["misses"] / base[k]["misses"] for k in keys]
            mg = sum((p[k]["lookup_us"] + p[k]["repl_us"]) / p[k]["total_us"] for k in keys) / len(keys)
            m60 = next((p[k]["mem_policy_bytes"] for k in keys if k[0] in (60, 75)), "")
            L.append(f"| {pname} | {gm(cr):.4f} | {gm(mr):.4f} | {min(cr):.3f} | {sum(c > 1.0005 for c in cr)} / {sum(c < 0.9995 for c in cr)} | {m60} | {100*mg:.2f}% |")
        L.append("")
    L += ["## Per-cell cost ratio vs LRU (all cells)", "", "| suite | workload | NBUF | Clock | Clock-NR | EVOLVED | misses LRU / EVOLVED |", "|---|---|--:|--:|--:|--:|--:|"]
    for split in ("train", "heldout"):
        b = idx(split, "LRU"); c = idx(split, "Clock"); n = idx(split, "Clock-NR"); e = idx(split, "EVOLVED")
        for k in sorted(b, key=lambda k: (k[1], k[0])):
            f = lambda d: f"{b[k]['total_us'] / d[k]['total_us']:.3f}"
            L.append(f"| {split} | {k[1]} | {k[0]} | {f(c)} | {f(n)} | {f(e)} | {b[k]['misses']} / {e[k]['misses']} |")
    open(out + ".md", "w").write("\n".join(L) + "\n")
    print("\n".join(L[:L.index('## Per-cell cost ratio vs LRU (all cells)')]))
    print(f"wrote {out}.csv and {out}.md")


if __name__ == "__main__":
    main()
