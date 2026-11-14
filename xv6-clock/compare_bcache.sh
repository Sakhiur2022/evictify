#!/bin/bash
# Host-side (no QEMU) comparison of LRU vs Clock vs Clock-NR, list and hash
# lookup.  Compiles the unmodified bio.c, runs the workload matrix, cross-checks
# every run against an independent Python model, writes ../results.{md,csv}.
#   bash compare_bcache.sh            full matrix
#   bash compare_bcache.sh --quick    reduced matrix
exec python3 "$(cd "$(dirname "$0")" && pwd)/bcsim/run_matrix.py" "$@"
