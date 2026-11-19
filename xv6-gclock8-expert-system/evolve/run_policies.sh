#!/usr/bin/env bash
# Score + compare all bundled policies on the host with plain gcc/python3 (NO QEMU, NO LLM, no network).
#   bash evolve/run_policies.sh            # selftest, score table, full comparison for the dual-expert policy
#   bash evolve/run_policies.sh --quick    # skip selftest + comparison, only the score table
set -euo pipefail
cd "$(dirname "$0")"
if [ "${1:-}" != "--quick" ]; then python3 selftest.py | tail -1; fi
python3 score_all.py
if [ "${1:-}" != "--quick" ]; then
  python3 compare_best.py seeds/dual_expert.h --out ../results_policies
  echo "wrote results_policies.md / .csv in the xv6 root"
fi
