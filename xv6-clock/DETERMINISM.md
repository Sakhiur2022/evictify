# Making the bcbench delay deterministic

## Why ticks varied (all found in this tree)
| # | Source | Where |
|---|--------|-------|
| 1 | `ticks` is driven by the LAPIC timer (`lapic.c`, TICR=10,000,000), which QEMU maps to *host wall-clock* time. Host load/IDE emulation speed changes how many ticks elapse. | `trap.c`, `lapic.c` |
| 2 | `start_ticks` was sampled at an arbitrary phase of a 10 ms period -> +/-1 tick quantization error. | `bio.c` |
| 3 | `CPUS` defaults to 2: IDE IRQ goes to CPU `ncpu-1`, timer ticks are counted on CPU 0 -> interleaving varies. | `Makefile`, `ide.c` |
| 4 | LRU order and block identities left from boot or a previous bcbench changed the lookup/victim-inspection counts and the victim picked. | `bio.c` |
| 5 | Disk contents/state could persist between boots. | `Makefile` |

## Changes
* `bio.c` (`bcache_bench`): free buffers get identity reset and are re-linked in canonical order before each run (replacement rule untouched); start is aligned to a tick edge.
* `Makefile`: new target `make qemu-nox-det` = `-accel tcg -icount shift=0,sleep=off -rtc clock=vm -snapshot -smp 1`.
  Virtual time then advances by executed instructions, not host time. `shift` is fixed on purpose (`shift=auto` adapts to host speed and is non-deterministic).
* `run_det.sh`: runs bcbench N times and diffs the stats.

## Use
    make clean && make qemu-nox-det
    $ bcbench 120 5
or `./run_det.sh 5 120 5` to check N runs automatically.

## Not verified / residual risk
This was built (`-Werror`) but **not run**: the authoring sandbox had no QEMU.
* icount makes the CPU + timer deterministic; QEMU's IDE completion goes through the block layer, which I have not confirmed is fully icount-bound on your QEMU version. `run_det.sh` is the test. If it fails, the fallback is QEMU record/replay (`-icount shift=0,rr=record|replay`) or a count-based logical delay.
* Tick values will differ from your old wall-clock numbers (virtual ns, not real ms); compare only within the deterministic mode.
* Other QEMU versions/hosts can legitimately give a different (but self-consistent) tick count.
