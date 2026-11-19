"""OpenEvolve evaluator for the xv6 buffer-cache replacement policy.

    evaluate(program_path) -> EvaluationResult      (called by OpenEvolve for every candidate)
    python evaluator.py FILE.h [--heldout-off]       (manual check of one policy file)

Pipeline
  1. lint      (lint.py)   -- no hidden state, no forbidden calls, frame untouched
  2. sanity    -- compiles with -Wall -Werror, passes the `toy` and `safety` workloads
                  (safety pins one buffer and leaves one dirty: evicting either is fatal)
  3. TRAIN suite   (suite.TRAIN x TRAIN_NBUF)     -> combined_score
  4. HELDOUT suite (suite.HELDOUT x HELDOUT_NBUF) -> reported only (over-fitting detector)

Score (all against the hook-LRU seed in the SAME framework, identical hash lookup, request
sequences, cold start and unit costs):
  r_c       = T_lru(c) / T_policy(c)          modeled total cost ratio for cell c   (>1: better than LRU)
  speedup   = geometric mean of r_c over the cells
  mem_frac  = policy state bytes / cache data bytes (mean over cache sizes)
  combined  = speedup * (1 + W*mem_frac_LRU) / (1 + W*mem_frac_policy),   W = MEM_WEIGHT   (LRU seed == 1.0)
"""
import hashlib, json, math, os, sys, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import harness, lint, suite

try:                                   # inside OpenEvolve
    from openevolve.evaluation_result import EvaluationResult
except Exception:                      # standalone use / tests without the package installed
    from dataclasses import dataclass, field

    @dataclass
    class EvaluationResult:
        metrics: dict
        artifacts: dict = field(default_factory=dict)

MEM_WEIGHT = float(os.environ.get("XV6_EVO_MEM_WEIGHT", "0.25"))   # 0 = ignore memory
DEFINES = ["-DBCACHE_EVOLVED", "-DBCACHE_LOOKUP_HASH"]
LRU_SEED = os.path.join(HERE, "seeds", "lru.h")
CACHE_DIR = os.path.join(HERE, ".cache")
INFEASIBLE = {"combined_score": 0.0, "valid": 0.0}


def _gm(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def _fail(stage, msg, extra=None):
    m = dict(INFEASIBLE); m["failed_stage"] = {"lint": 1.0, "build": 2.0, "sanity": 3.0, "suite": 4.0}[stage]
    a = {"error": f"{stage} failed", "details": msg[-2500:]}
    if extra: a.update(extra)
    return EvaluationResult(metrics=m, artifacts=a)


def _suite_runs(tmp, tag, policy, nbufs, cfgs):
    """-> {(name, nbuf): csv-dict}"""
    out = {}
    for nb in nbufs:
        exe = harness.build(tmp, f"{tag}_{nb}", nb, DEFINES, policy=policy)
        for name, cfg in cfgs:
            out[(name, nb)] = harness.run(exe, cfg)
    return out


def _baseline(tmp, which):
    """LRU-seed results for a suite, cached on disk (keyed by the sources that determine them)."""
    nbufs, cfgs = (suite.TRAIN_NBUF, suite.TRAIN) if which == "train" else (suite.HELDOUT_NBUF, suite.HELDOUT)
    h = hashlib.sha1()
    for p in (os.path.join(harness.SRC, "bio.c"), LRU_SEED, os.path.join(HERE, "suite.py")):
        h.update(open(p, "rb").read())
    key = os.path.join(CACHE_DIR, f"lru_{which}_{h.hexdigest()[:12]}.json")
    if os.path.exists(key):
        d = json.load(open(key))
        return {tuple([k.split("|")[0], int(k.split("|")[1])]): v for k, v in d.items()}
    res = _suite_runs(tmp, f"base_{which}", LRU_SEED, nbufs, cfgs)
    os.makedirs(CACHE_DIR, exist_ok=True)
    tmpf = key + f".{os.getpid()}.tmp"
    json.dump({f"{k[0]}|{k[1]}": v for k, v in res.items()}, open(tmpf, "w"))
    os.replace(tmpf, key)
    return res


def _score(pol, base, nbufs, cfgs):
    rows, r_all, m_all, mg = [], [], [], []
    for nb in nbufs:
        for name, _ in cfgs:
            p, b = pol[(name, nb)], base[(name, nb)]
            r = b["total_us"] / p["total_us"]
            m = p["misses"] / b["misses"]
            r_all.append(r); m_all.append(m)
            mg.append((p["lookup_us"] + p["repl_us"]) / p["total_us"])
            rows.append((name, nb, r, m, p["misses"], b["misses"]))
    return dict(speedup=_gm(r_all), miss_ratio=_gm(m_all), worst=min(r_all), mgmt=sum(mg) / len(mg), rows=rows,
                wins=sum(1 for r in r_all if r > 1.0005), losses=sum(1 for r in r_all if r < 0.9995), n=len(r_all))


def evaluate(program_path, heldout=True):
    src = open(program_path).read()
    ref = open(LRU_SEED).read()
    # 1. lint ------------------------------------------------------------------------------
    probs = lint.check(src, frame_reference=ref)
    if probs:
        return _fail("lint", "\n".join(probs), {"hint": "See the ALLOWED INTERFACE comment at the top of the file."})
    tmp = tempfile.mkdtemp(prefix="xv6evo_")
    try:
        # 2. sanity ---------------------------------------------------------------------------
        try:
            exe = harness.build(tmp, "sanity", suite.SANITY_NBUF, DEFINES, policy=program_path)
        except harness.BuildError as e:
            return _fail("build", str(e))
        for label, cfg in (("toy", (0, 1, 1, 1, 1, 1)), ("safety", (6, 1, 1, 1, 1, 1))):
            try:
                harness.run(exe, cfg)
            except harness.RunError as e:
                return _fail("sanity", f"workload '{label}': {e}",
                             {"hint": "pol_victim must return a free buffer (use pol_cand_ok); hooks must not loop forever."})
        # 3. train suite ----------------------------------------------------------------------
        try:
            pol_tr = _suite_runs(tmp, "tr", program_path, suite.TRAIN_NBUF, suite.TRAIN)
        except (harness.BuildError, harness.RunError) as e:
            return _fail("suite", str(e))
        tr = _score(pol_tr, _baseline(tmp, "train"), suite.TRAIN_NBUF, suite.TRAIN)
        mem_frac = sum(pol_tr[(suite.TRAIN[0][0], nb)]["mem_policy"] / (nb * suite.BLOCK_BYTES) for nb in suite.TRAIN_NBUF) / len(suite.TRAIN_NBUF)
        base_tr = _baseline(tmp, "train")
        mem_frac_lru = sum(base_tr[(suite.TRAIN[0][0], nb)]["mem_policy"] / (nb * suite.BLOCK_BYTES) for nb in suite.TRAIN_NBUF) / len(suite.TRAIN_NBUF)
        mem_factor = (1 + MEM_WEIGHT * mem_frac_lru) / (1 + MEM_WEIGHT * mem_frac)
        combined = tr["speedup"] * mem_factor
        metrics = {
            "combined_score": combined, "valid": 1.0,
            "speedup_gm_train": tr["speedup"], "miss_ratio_gm_train": tr["miss_ratio"],
            "worst_cell_speedup": tr["worst"], "cells_better_than_lru": float(tr["wins"]), "cells_worse_than_lru": float(tr["losses"]),
            "mem_policy_pct_of_data": 100 * mem_frac, "mgmt_share_pct": 100 * tr["mgmt"],
        }
        art = {"train_cells": "workload | NBUF | cost ratio vs LRU (>1 better) | misses policy/LRU\n" +
               "\n".join(f"{n} | {nb} | {r:.3f} | {pm}/{bm}" for n, nb, r, m, pm, bm in sorted(tr["rows"], key=lambda x: x[2])[:10]),
               "note": "10 worst training cells shown"}
        # 4. held-out -------------------------------------------------------------------------
        if heldout:
            try:
                pol_ho = _suite_runs(tmp, "ho", program_path, suite.HELDOUT_NBUF, suite.HELDOUT)
                ho = _score(pol_ho, _baseline(tmp, "heldout"), suite.HELDOUT_NBUF, suite.HELDOUT)
                metrics.update({"speedup_gm_heldout": ho["speedup"], "miss_ratio_gm_heldout": ho["miss_ratio"],
                                "worst_cell_speedup_heldout": ho["worst"],
                                "generalisation_gap": tr["speedup"] - ho["speedup"]})
                art["heldout_worst_cells"] = "\n".join(f"{n} | {nb} | {r:.3f}" for n, nb, r, m, pm, bm in sorted(ho["rows"], key=lambda x: x[2])[:5])
            except (harness.BuildError, harness.RunError) as e:
                art["heldout_error"] = str(e)[-500:]
        return EvaluationResult(metrics=metrics, artifacts=art)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    r = evaluate(sys.argv[1], heldout="--heldout-off" not in sys.argv)
    print(json.dumps(r.metrics, indent=2))
    for k, v in r.artifacts.items():
        print(f"--- {k}\n{v}")
