#!/bin/bash
# Run bcbench N times under deterministic QEMU and compare the results.
# Usage: ./run_det.sh [runs=5] [scan_blocks=120] [rounds=5] [NBUF=]
# Needs qemu on the host. Exit 0 = every run printed identical stats.
RUNS=${1:-5}; SCAN=${2:-120}; ROUNDS=${3:-5}; NB=${4:-}
cd "$(dirname "$0")" || exit 1
make clean >/dev/null 2>&1
make ${NB:+NBUF=$NB} fs.img xv6.img >/dev/null 2>&1 || { echo "build failed"; exit 1; }
QEMU=${QEMU:-$(command -v qemu-system-i386 || command -v qemu)}
OPTS="-nographic -drive file=fs.img,index=1,media=disk,format=raw \
 -drive file=xv6.img,index=0,media=disk,format=raw -m 512 \
 -accel tcg -icount shift=0,sleep=off -rtc clock=vm -snapshot -smp 1"
mkdir -p det_out
for i in $(seq 1 "$RUNS"); do
  # Keystroke arrival time is host wall-clock; the kernel tick-aligns the
  # benchmark start so this does not leak into the result.
  ( sleep 4; echo "bcbench $SCAN $ROUNDS"; sleep 8 ) \
    | timeout 60 $QEMU $OPTS 2>&1 | tr -d '\r' > det_out/run$i.log
  grep -E "Delay|Approx|Requests|hits|misses|inspections|Total Compute" \
       det_out/run$i.log > det_out/run$i.stats
  echo "run $i: $(grep -m1 'Delay:' det_out/run$i.log)"
done
if [ -s det_out/run1.stats ] && \
   [ "$(md5sum det_out/run*.stats | awk '{print $1}' | sort -u | wc -l)" = 1 ]; then
  echo "DETERMINISTIC: all $RUNS runs identical"; exit 0
fi
echo "NOT DETERMINISTIC (or no output) - diff det_out/run*.stats"; exit 2
