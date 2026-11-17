"""Workload suites used to score policies.

TRAIN   -> drives `combined_score` (what OpenEvolve optimises).
HELDOUT -> different parameters, seeds AND cache sizes; only reported.  A big gap between the two
           means the policy has over-fitted the training workloads.

cfg = (workload, scan, hot, interval, passes, rounds)
workload ids: 1 seq, 2 hotscan, 3 wset, 4 pollute, 5 mixed, 7 zipf (skewed random), 8 loop (cyclic sweep)
"""
TRAIN_NBUF = [30, 60, 90]
TRAIN = [
    ("hotscan s120",   (2, 120,  4, 40, 3, 5)),
    ("hotscan s240",   (2, 240,  4, 40, 3, 5)),
    ("pollute s40",    (4,  40,  8, 40, 3, 5)),
    ("pollute s120",   (4, 120,  8, 40, 3, 5)),
    ("mixed s120",     (5, 120,  8, 40, 3, 5)),
    ("mixed s240",     (5, 240,  8, 40, 3, 5)),
    ("wset h48",       (3,   1, 48, 40, 3, 5)),
    ("seq s240",       (1, 240,  4, 40, 3, 3)),
    ("zipf U300 k2",   (7, 300, 20,  7, 2, 15)),
    ("zipf U200 k3",   (7, 200, 20,  3, 3, 20)),
    ("loop L45",       (8,  45,  4, 40, 3, 6)),
    ("loop L100",      (8, 100,  4, 40, 3, 4)),
]
HELDOUT_NBUF = [45, 75, 120]
HELDOUT = [
    ("hotscan s180 I30",  (2, 180,  6, 30, 2, 5)),
    ("hotscan s300 I50",  (2, 300,  6, 50, 3, 4)),
    ("pollute s80",       (4,  80, 12, 40, 3, 5)),
    ("mixed s160 I30",    (5, 160, 12, 30, 3, 5)),
    ("wset h36",          (3,   1, 36, 40, 3, 6)),
    ("seq s300",          (1, 300,  4, 40, 3, 3)),
    ("zipf U350 k2",      (7, 350, 30, 21, 2, 15)),
    ("zipf U150 k4",      (7, 150, 10, 99, 4, 25)),
    ("loop L70",          (8,  70,  4, 40, 3, 6)),
    ("loop L130",         (8, 130,  4, 40, 3, 4)),
]
SANITY_NBUF = 30                     # toy + safety checks are run here first
BLOCK_BYTES = 512
