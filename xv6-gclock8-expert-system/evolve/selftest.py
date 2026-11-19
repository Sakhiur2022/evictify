#!/usr/bin/env python3
"""Self-test of the policy-hook framework.

 1. The three seed policies (LRU / Clock / Clock-NR written in the hook API) must reproduce the
    NATIVE bio.c policies exactly: hits, misses, hot hits, hot evictions, victim inspections.
 2. The native policies must agree with the independent Python model on the new workloads.
 3. Invalid policies must be rejected (busy victim, out of range, forbidden constructs).
Usage: python3 evolve/selftest.py
"""
import os, sys, tempfile, shutil
HERE = os.path.dirname(os.path.abspath(__file__)); SRC = os.path.dirname(HERE)
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(SRC, "bcsim"))
import harness, refmodel, lint

CFGS = [(0, 1, 1, 1, 1, 1), (1, 240, 4, 40, 3, 3), (2, 120, 4, 40, 3, 5), (3, 1, 24, 40, 3, 5), (4, 40, 8, 40, 3, 4),
        (5, 120, 8, 40, 3, 3), (7, 200, 20, 11, 2, 6), (7, 300, 10, 5, 3, 6), (8, 45, 4, 40, 3, 6), (8, 100, 4, 40, 3, 4)]
NBUFS = [30, 60, 90]
KEYS = ["requests", "hits", "misses", "hot_req", "hot_hits", "vic_insp", "hot_evict", "ws_probes", "ws_res"]
SEEDS = {"LRU": "lru.h", "CLOCK": "clock.h", "CLOCK-NR": "clock_nr.h"}
NATIVE = {"LRU": ["-DBCACHE_LRU"], "CLOCK": [], "CLOCK-NR": ["-DBCACHE_CLOCK_NOREF_ON_LOAD"]}

def main():
    tmp = tempfile.mkdtemp(); bad = 0; n = 0
    try:
        for nb in NBUFS:
            nat = {p: harness.build(tmp, f"nat_{p}", nb, NATIVE[p] + ["-DBCACHE_LOOKUP_HASH"]) for p in NATIVE}
            seed = {p: harness.build(tmp, f"seed_{p}", nb, ["-DBCACHE_EVOLVED", "-DBCACHE_LOOKUP_HASH"],
                                     policy=os.path.join(HERE, "seeds", SEEDS[p])) for p in SEEDS}
            for cfg in CFGS:
                for p in SEEDS:
                    a = harness.run(nat[p], cfg); b = harness.run(seed[p], cfg); n += 1
                    for k in KEYS:
                        if a[k] != b[k]:
                            bad += 1; print(f"SEED MISMATCH {p} nbuf={nb} cfg={cfg} {k}: native={a[k]} seed={b[k]}")
                    if cfg[0] in (7, 8):            # native vs independent Python model on new workloads
                        m = refmodel.simulate(p, nb, refmodel.gen(cfg[0], nb, *cfg[1:5], cfg[5]), cfg[2])
                        for ck, mk in [("hits", "hits"), ("misses", "misses"), ("hot_req", "hot_req"), ("hot_hits", "hot_hits"),
                                       ("vic_insp", "vic_insp"), ("hot_evict", "hot_evict")]:
                            if int(a[ck]) != m[mk]:
                                bad += 1; print(f"MODEL MISMATCH {p} nbuf={nb} cfg={cfg} {ck}: C={a[ck]} model={m[mk]}")
        print(f"seed-vs-native comparisons: {n} runs x {len(KEYS)} counters")
        # negative tests --------------------------------------------------------------------
        base = open(os.path.join(HERE, "seeds", "lru.h")).read()
        cases = {
          "returns busy victim": base.replace("if(pol_cand_ok(i))\n      return i;", "return i;"),
          "global variable": base.replace("static void\nlru_unlink", "int cheat_counter;\nstatic void\nlru_unlink"),
          "direct storage access": base.replace("lru_unlink(int i)\n{", "lru_unlink(int i)\n{\n  pol_pm[i].older = 0;"),
          "forbidden call": base.replace("(void)blockno;\n  for(i = G_GET(tail)", "(void)blockno; cprintf(\"x\");\n  for(i = G_GET(tail)"),
          "preprocessor": base.replace("static void\nlru_unlink", "#define FREE 1\nstatic void\nlru_unlink"),
          "64-bit type": base.replace("int nw = P_GET(i, newer)", "long long z = 0; int nw = P_GET(i, newer)"),
          "block-number constant": base.replace("(void)blockno;\n  for(i = G_GET(tail)", "if(blockno >= 500 && blockno < 564) { }\n  for(i = G_GET(tail)"),
          "edit outside blocks": base.replace("#define G_GET(f)       (pol_ops.reads++,  pol_pg.f)", "#define G_GET(f)       (pol_pg.f)"),
        }
        for name, src in cases.items():
            path = os.path.join(tmp, "neg.h"); open(path, "w").write(src)
            probs = lint.check(src, frame_reference=base)
            if not probs:        # not caught by lint -> must at least be caught at run time
                try:
                    exe = harness.build(tmp, "neg", 60, ["-DBCACHE_EVOLVED", "-DBCACHE_LOOKUP_HASH"], policy=path)
                    harness.run(exe, (2, 120, 4, 40, 3, 2)); harness.run(exe, (6, 1, 1, 1, 1, 1)); verdict = "NOT REJECTED"; bad += 1
                except Exception as e:
                    verdict = "rejected at build/run time"
            else:
                verdict = "rejected by lint: " + probs[0]
            print(f"  negative test [{name}]: {verdict}")
    finally:
        shutil.rmtree(tmp)
    print("SELFTEST", "PASSED" if bad == 0 else f"FAILED ({bad} problems)")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
