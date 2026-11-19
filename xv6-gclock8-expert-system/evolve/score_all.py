#!/usr/bin/env python3
"""Score every policy in seeds/ (or the files given) on the train + held-out suites, no QEMU.

    python3 score_all.py                 # all seeds
    python3 score_all.py my_policy.h     # specific files
Prints one table; ratios are LRU cost / policy cost (>1 beats LRU).  Host build of the unmodified bio.c.
"""
import glob, os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import evaluator

files = sys.argv[1:] or sorted(glob.glob(os.path.join(HERE, "seeds", "*.h")))
print(f"{'policy':<24}{'combined':>9}{'train':>8}{'heldout':>9}{'worst':>7}{'miss(tr)':>9}{'state%':>8}")
for f in files:
    m = evaluator.evaluate(f).metrics
    if "combined_score" not in m or m.get("combined_score", 0) == 0:
        print(f"{os.path.basename(f):<24} FAILED"); continue
    print(f"{os.path.basename(f):<24}{m['combined_score']:>9.3f}{m['speedup_gm_train']:>8.3f}{m['speedup_gm_heldout']:>9.3f}"
          f"{m['worst_cell_speedup']:>7.3f}{m['miss_ratio_gm_train']:>9.3f}{m['mem_policy_pct_of_data']:>8.2f}")
