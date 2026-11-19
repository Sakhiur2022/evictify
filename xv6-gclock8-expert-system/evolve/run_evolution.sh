#!/usr/bin/env bash
# Evolve the xv6 buffer-cache replacement policy with OpenEvolve.
#   bash evolve/run_evolution.sh [iterations] [extra openevolve args...]
# Needs: python>=3.10, `pip install -e third_party/openevolve`, gcc, and an LLM backend (see config.yaml).
set -euo pipefail
cd "$(dirname "$0")"
ITER="${1:-300}"; shift || true
python3 selftest.py                                   # the framework must be healthy before spending LLM budget
python3 ../third_party/openevolve/openevolve-run.py seeds/lru.h evaluator.py \
        --config config.yaml --iterations "$ITER" --output openevolve_output "$@"
echo "Best program: openevolve_output/best/best_program.h"
echo "Next:  python3 compare_best.py openevolve_output/best/best_program.h"
