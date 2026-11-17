#!/usr/bin/env bash
# Install an evolved policy into the xv6 source tree and build it.
#   bash evolve/install_best.sh evolve/openevolve_output/best/best_program.h [NBUF]
set -euo pipefail
cd "$(dirname "$0")/.."
cp "$1" policy_evolve.h
make clean >/dev/null 2>&1 || true
make NBUF="${2:-60}" CFLAGS_EXTRA="-DBCACHE_EVOLVED -DBCACHE_LOOKUP_HASH" kernel
echo "built kernel with the evolved policy; in xv6 run:  bcbench pollute scan=120 hot=8"
