#!/usr/bin/env python3
"""Independent reference model of the benchmark (NOT derived from bio.c).

Re-implements the workload generators and the LRU / Clock / Clock-NR policies
in Python so the C counters can be cross-validated.  Single-threaded, no
pinned/dirty buffers (the benchmark's own assumption).
"""
SCAN_START, REGION, HOT_START, WARM_START, TOY_START = 100, 400, 500, 600, 600

def gen(w, nbuf, scan, hot, interval, passes, rounds):
    """Yield (block, is_hot) exactly as the workload definitions say."""
    seq = [0]
    first = [True]
    out = []
    def req(b, h=0): out.append((b, h))
    def nxt():
        b = SCAN_START + seq[0] % REGION; seq[0] += 1; return b
    def hotphase(p):
        out.append(("PROBE", first[0])); first[0] = False
        for _ in range(p):
            for j in range(hot): req(HOT_START + j, 1)
    if w == 0:
        for i in range(nbuf): req(TOY_START + i)
        req(TOY_START); req(TOY_START + nbuf); req(TOY_START)
    elif w == 1:
        for _ in range(rounds):
            for _ in range(scan): req(nxt())
    elif w == 2:
        for _ in range(rounds):
            for i in range(scan):
                req(SCAN_START + i)
                if (i + 1) % interval == 0: hotphase(passes)
    elif w == 3:
        for _ in range(rounds): hotphase(passes)
    elif w == 4:
        for _ in range(rounds):
            hotphase(passes)
            for _ in range(scan): req(nxt())
        hotphase(passes)
    elif w == 5:
        for _ in range(rounds):
            hotphase(passes)
            base = seq[0]
            for i in range(scan):
                req(nxt())
                if (i + 1) % interval == 0: hotphase(1)
            r = min(hot, scan)
            for j in range(r): req(SCAN_START + (base + scan - r + j) % REGION)
            for j in range(2 * hot): req(WARM_START + j)
    elif w == 6:
        for i in range(2 * nbuf + 5): req(TOY_START + 2 + i)   # pin/dirty parts not modelled
    return out

def simulate(policy, nbuf, reqs, hot):
    """policy in LRU | CLOCK | CLOCK-NR.  List-mode lookup cost is modelled."""
    n = nbuf
    where = {}                       # block -> buffer index
    blk = [None] * n
    ref = [0] * n
    lst = list(range(n - 1, -1, -1)) # MRU ... LRU (canonical cold start)
    hand = 0
    s = dict(requests=0, hits=0, misses=0, hot_req=0, hot_hits=0, ws_probes=0,
             ws_res=0, lookup_insp=0, vic_insp=0, hot_evict=0, ref_clears=0,
             ref_checks=0, ref_set_hit=0, ref_set_load=0, lru_ptrw=0)
    for b, h in reqs:
        if b == "PROBE":
            if not h:
                s["ws_probes"] += hot
                s["ws_res"] += sum(1 for j in range(hot) if HOT_START + j in where)
            continue
        s["requests"] += 1
        if policy == "LRU":
            pos = lst.index(where[b]) + 1 if b in where else n
        else:
            pos = n - where[b] if b in where else n   # static list: buf[n-1] is head.next
        s["lookup_insp"] += pos
        if b in where:
            s["hits"] += 1
            i = where[b]
            if policy != "LRU":
                ref[i] = 1; s["ref_set_hit"] += 1
            if h: s["hot_req"] += 1; s["hot_hits"] += 1
        else:
            s["misses"] += 1
            if h: s["hot_req"] += 1
            if policy == "LRU":
                i = lst[-1]; s["vic_insp"] += 1
            else:
                while True:
                    i = hand; hand = (hand + 1) % n
                    s["vic_insp"] += 1; s["ref_checks"] += 1
                    if ref[i]:
                        ref[i] = 0; s["ref_clears"] += 1
                    else:
                        break
                ref[i] = 0 if policy == "CLOCK-NR" else 1
                if policy == "CLOCK": s["ref_set_load"] += 1
            old = blk[i]
            if old is not None:
                del where[old]
                if HOT_START <= old < HOT_START + hot: s["hot_evict"] += 1
            blk[i] = b; where[b] = i
            if h: pass
        if policy == "LRU":
            lst.remove(where[b]); lst.insert(0, where[b]); s["lru_ptrw"] += 6
    return s
