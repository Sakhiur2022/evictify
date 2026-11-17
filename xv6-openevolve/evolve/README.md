# Evolving the xv6 buffer-cache replacement policy with OpenEvolve

OpenEvolve (vendored in `../third_party/openevolve`, Apache-2.0) asks an LLM to rewrite the
**replacement policy** in `policy_evolve.h`; `evaluator.py` scores every candidate by running the real
`bio.c` benchmark; the best candidates breed the next generation.

```
OpenEvolve  --mutates-->  policy_evolve.h  --compiled into-->  bio.c  --runs-->  workloads  --> cost model --> score
   ^                       (2 EVOLVE blocks)   (fixed: lookup, locking,            (train + held-out)            |
   +------------------------------------------ counters, safety checks) ------------------------------------------+
```

## What is evolvable, what is not
* **Evolvable:** `struct pol_meta` (per-buffer state), `struct pol_global`, and five hooks
  `pol_init / pol_on_hit / pol_on_load / pol_on_release / pol_victim`.
* **Fixed (cannot be changed; reverted by `enforce_evolve_blocks`):** hash lookup, locking, hit/miss accounting,
  victim validation (a busy or dirty victim is fatal), the counting macros, and the cost model.
* The lookup is always the hash table, so a policy is never rewarded for changing the lookup structure.

## Files
| file | purpose |
|---|---|
| `seeds/lru.h` | default xv6 LRU in the hook API (**the initial program**; scores 1.000 by definition) |
| `seeds/clock.h`, `seeds/clock_nr.h` | Clock (bit set on load) and Clock-NR (bit set on re-reference); seed islands 1 and 2 |
| `seeds/example_ghost_clock.h` | hand-written example with ghost entries, used to test the pipeline (NOT an evolved result) |
| `evaluator.py` | `evaluate(program_path)` -> metrics + artifacts for OpenEvolve; also a CLI |
| `suite.py` | train and held-out workload/cache-size suites |
| `lint.py` | rules that stop a program hiding state or work from the cost model |
| `harness.py` | build/run helpers (host build of the unmodified `bio.c`) |
| `selftest.py` | proves the framework is faithful and the guards work (run before every evolution) |
| `compare_best.py` | final table: evolved policy vs LRU / Clock / Clock-NR on train + held-out |
| `config.yaml`, `run_evolution.sh`, `install_best.sh` | run OpenEvolve; put the winner into the kernel |

## Run
```bash
pip install -e third_party/openevolve         # python>=3.10 (needs network + an LLM backend, see config.yaml)
bash evolve/run_evolution.sh 300              # runs selftest.py, then OpenEvolve
python3 evolve/compare_best.py evolve/openevolve_output/best/best_program.h
bash evolve/install_best.sh evolve/openevolve_output/best/best_program.h 60    # kernel build (needs -m32 toolchain)
```
Manual check of any policy file: `python3 evolve/evaluator.py path/to/policy.h`.

## The score
For each cell *c* (workload x cache size) the evaluator measures the modeled total cost `T` (same model as the
benchmark: miss 4000 us, hit 10 us, every inspection/read 1 us, every write 1 us) and compares with the LRU seed run
through the identical framework:

    r_c = T_LRU(c) / T_policy(c)          speedup = geometric mean of r_c over the training cells
    combined_score = speedup * (1 + w*f_LRU) / (1 + w*f_policy)       f = policy state bytes / cache data bytes, w = 0.25

`w` (`XV6_EVO_MEM_WEIGHT`, default 0.25, 0 = ignore memory) trades speed against memory. **Changing `w` can change the
ranking** (at w=1.0, plain Clock out-scores LRU on memory alone although it is slower), so state it when you report results.

Metrics returned: `combined_score`, `speedup_gm_train`, `miss_ratio_gm_train`, `worst_cell_speedup`, `cells_better/worse_than_lru`,
`mem_policy_pct_of_data`, `mgmt_share_pct`, and (not part of the fitness) `speedup_gm_heldout`, `miss_ratio_gm_heldout`,
`generalisation_gap`. MAP-Elites axes: `miss_ratio_gm_train` x `mgmt_share_pct`.

## What counts as cheating (and how it is stopped)
| attempt | defence |
|---|---|
| change the counters, cost macros or checks | text outside the EVOLVE blocks is reverted by OpenEvolve and re-checked by `lint.py` |
| keep state in globals / static locals / direct array access | lint rejects it; all state must be in `pol_meta`/`pol_global`, whose bytes are charged |
| hide work in libc / kernel calls, `#define`, asm | forbidden identifiers and preprocessor lines are rejected |
| return a busy or dirty buffer | the core validates every victim; the `safety` workload pins and dirties buffers |
| skip inspecting candidates | the core charges at least one inspection per miss |
| tune to the benchmark's block numbers or to NBUF values | numeric literals that look like block ids are rejected; the held-out suite uses other parameters, seeds and cache sizes (45/75/120) |

## Honest limits
* All costs are **modeled** (operation counts x unit costs), not measured time; the host build uses the same `bio.c`
  but not QEMU. Re-run the winner inside xv6 (`bcbench ...`) to confirm the counters.
* The guards are static and conservative, not a proof of honesty: **read the winning code** before trusting its score. Pure
  arithmetic inside a hook (no P_GET/P_SET) is free in the model; the 10 s run timeout stops runaway code but not mild abuse.
* Workloads are synthetic and deterministic. A policy that wins here may not win on real file-system traces.
* Reads of policy state are charged (1 us each), so hook-based Clock costs slightly more bookkeeping than the native Clock in
  `bio.c`; every candidate and baseline is charged the same way, and miss counts equal the native ones exactly (`selftest.py`).
* The kernel (`-m32 -Werror`) build of an evolved policy could not be tested in the authoring sandbox (no 32-bit toolchain); the
  host build uses the same warning flags and 32-bit-safe rules, but compile it in your xv6 tree before relying on it.
* No LLM run has been performed by the author of this integration; the baseline scores below are the only measured numbers.

## Baseline scores (measured with `evaluator.py`; train / held-out)
| policy | combined | train cost ratio | held-out cost ratio | train miss ratio | policy state / data |
|---|--:|--:|--:|--:|--:|
| LRU seed | 1.000 | 1.000 | 1.000 | 1.000 | 1.59% |
| Clock (bit on load) | 0.994 | 0.991 | 0.991 | 1.013 | 0.21% |
| Clock-NR | 1.022 | 1.019 | 1.023 | 0.986 | 0.21% |

The hand-written ghost-list example scores 1.010 (it pays for scanning its ghost ring on every miss). Any evolved policy should beat Clock-NR (combined about 1.022) to be interesting.
