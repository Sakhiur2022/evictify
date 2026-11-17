// policy_evolve.h -- buffer-cache replacement policy (target of OpenEvolve).
//
// HOW THIS FILE IS USED
//   bio.c includes this file when built with -DBCACHE_EVOLVED -DBCACHE_LOOKUP_HASH.
//   Lookup, locking, hit/miss accounting and safety checks are done by bio.c and are
//   NOT part of the policy.  The policy decides only WHICH buffer to recycle.
//
// HOOKS (all run with bcache.lock held; never sleep, never allocate, no I/O)
//   pol_init()                       reset your state (storage is already zeroed).
//   pol_on_hit(i)                    block cached in buffer i was requested again.
//   pol_on_load(i, blockno, old)     buffer i now holds `blockno`; `old` was its previous
//                                    block (0xffffffff if it held nothing): ghost-list hook.
//   pol_on_release(i)                buffer i was released (refcnt reached 0).
//   pol_victim(blockno)              a miss on `blockno`: return the index (0..NBUF-1) of a
//                                    buffer to recycle.  The returned buffer MUST be free:
//                                    call pol_cand_ok(i) on candidates; it returns 1 only if
//                                    buffer i has refcnt==0 and is not dirty.  Returning a
//                                    busy or dirty buffer is a fatal error.
//
// ALLOWED INTERFACE (everything else is rejected by the evaluator)
//   pol_cand_ok(i)    inspect candidate i                      cost: 1 inspection
//   pol_now()         logical time = number of requests so far  cost: free
//   P_GET(i, f)       read field f of per-buffer state i        cost: 1 read
//   P_SET(i, f, v)    write field f of per-buffer state i       cost: 1 write
//   G_GET(f)          read global policy field f                cost: 1 read
//   G_SET(f, v)       write global policy field f               cost: 1 write
//   ALL persistent policy state must live in struct pol_meta (one per buffer) and
//   struct pol_global (one per cache), accessed ONLY through the macros above.
//   Memory used by these two structs is measured and reported.
//   NBUF (compile-time constant, 30..120 in tests) is available.
//   Use only 32-bit integer types (int, uint, unsigned char, short): the code must
//   also compile into the 32-bit xv6 kernel with -Wall -Werror (no unused functions).
//
// COST MODEL (identical for every policy; see BCBENCH_USAGE.txt)
//   miss = 4000us, hit = 10us, each inspection/read = 1us, each write = 1us.
//   So avoiding a miss is worth ~4000 operations of bookkeeping.

#define P_GET(i, f)    (pol_ops.reads++,  pol_pm[(i)].f)
#define P_SET(i, f, v) (pol_ops.writes++, pol_pm[(i)].f = (v))
#define G_GET(f)       (pol_ops.reads++,  pol_pg.f)
#define G_SET(f, v)    (pol_ops.writes++, pol_pg.f = (v))

// EVOLVE-BLOCK-START
// Per-buffer and global policy state (evolvable).
// EXAMPLE (hand-written, not evolved): Clock-NR plus a ring of recently evicted block ids.
// A block that comes back while still in the ghost ring was evicted too early, so it is
// admitted with its reference bit already set (it must survive one extra sweep).
struct pol_meta {
  unsigned char ref;
};
struct pol_global {
  int hand;
  int gpos;              // next ghost slot to overwrite
  uint ghost[NBUF];      // ids of recently evicted blocks (0xffffffff = empty)
};
// EVOLVE-BLOCK-END

static struct pol_meta   pol_pm[NBUF];
static struct pol_global pol_pg;

// EVOLVE-BLOCK-START
// Policy hooks (evolvable).
static void
pol_init(void)
{
  int k;

  G_SET(hand, 0);
  G_SET(gpos, 0);
  for(k = 0; k < NBUF; k++)
    G_SET(ghost[k], 0xffffffff);
}

static void
pol_on_hit(int i)
{
  P_SET(i, ref, 1);
}

static void
pol_on_load(int i, uint blockno, uint old)
{
  int k, g = 0, p;

  for(k = 0; k < NBUF; k++)                // was this block evicted recently?
    if(G_GET(ghost[k]) == blockno){
      G_SET(ghost[k], 0xffffffff);
      g = 1;
      break;
    }
  P_SET(i, ref, g);                       // ghost hit: admit with the bit set
  if(old != 0xffffffff){                  // remember the block we just displaced
    p = G_GET(gpos);
    G_SET(ghost[p], old);
    G_SET(gpos, (p + 1) % NBUF);
  }
}

static void
pol_on_release(int i)
{
  (void)i;
}

static int
pol_victim(uint blockno)
{
  int h = G_GET(hand), n, i;

  (void)blockno;
  for(n = 0; n < 2 * NBUF; n++){
    i = h;
    h = (h + 1) % NBUF;
    if(!pol_cand_ok(i))
      continue;
    if(P_GET(i, ref)){
      P_SET(i, ref, 0);
      continue;
    }
    G_SET(hand, h);
    return i;
  }
  return 0;
}
// EVOLVE-BLOCK-END
