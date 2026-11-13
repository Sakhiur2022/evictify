#!/bin/bash
# Exact-repeatable measured ticks via QEMU record/replay.
# Usage: bash rr_det.sh [replays=3] [blocks=120] [rounds=5]
# Records ONE run of bcbench (auto-started by init, no keystrokes), then replays
# it N times. Every replay reproduces the recorded execution bit-for-bit, so the
# measured tick count is identical each time. Re-record after any code change.
set -e
N=${1:-3}; BLK=${2:-120}; RND=${3:-5}
SRC=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); cp -r "$SRC"/. "$W"; cd "$W"
make clean >/dev/null 2>&1 || true
python3 - "$BLK" "$RND" <<'PY'
import sys
b,r=sys.argv[1:3]
s=open('init.c').read()
s=s.replace('for(;;){\n    printf(1, "init: starting sh\\n");',
 'for(;;){\n    { int p=fork(); if(p==0){ char *a[]={"bcbench","%s","%s",0}; exec("bcbench",a); exit(); } wait(); }\n    printf(1, "init: starting sh\\n");'%(b,r),1)
open('init.c','w').write(s)
PY
make fs.img xv6.img >/dev/null 2>&1 || { echo "build failed"; exit 1; }
DRV="-drive file=fs.img,if=none,snapshot=on,format=raw,id=d1 -drive driver=blkreplay,if=none,image=d1,id=d1r -device ide-hd,drive=d1r,bus=ide.0,unit=1 -drive file=xv6.img,if=none,snapshot=on,format=raw,id=d0 -drive driver=blkreplay,if=none,image=d0,id=d0r -device ide-hd,drive=d0r,bus=ide.0,unit=0"
BASE="-display none -monitor none -serial stdio -m 512 -accel tcg -smp 1 -rtc clock=vm"
ICOUNT="shift=6"
run(){  # run(mode, logfile): start QEMU, stop it once the result prints
  qemu-system-i386 $BASE $DRV -icount $ICOUNT,rr=$1,rrfile=rr.bin </dev/null >"$2" 2>&1 &
  local pid=$!
  for _ in $(seq 1 200); do
    grep -q "^Wall-clock" "$2" 2>/dev/null && break
    kill -0 $pid 2>/dev/null || break
    sleep 1
  done
  sleep 1; kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
}
echo "== RECORD =="
run record rec.log
tr -d '\r' < rec.log | grep -E "^(Policy|Cache hits|Cache misses|Hit rate|Delay|Wall-clock)"
for i in $(seq 1 "$N"); do
  run replay rep$i.log
  echo "== REPLAY $i =="
  tr -d '\r' < rep$i.log | grep -E "^Wall-clock"
done
cp rr.bin "$SRC/rr_last.bin"; rm -rf "$W"
