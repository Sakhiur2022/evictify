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
// LRU seed: doubly linked recency list expressed with buffer indices.
struct pol_meta {
  int newer;   // neighbour towards MRU, -1 at the head
  int older;   // neighbour towards LRU, -1 at the tail
};
struct pol_global {
  int head;    // MRU buffer
  int tail;    // LRU buffer
};
// EVOLVE-BLOCK-END

static struct pol_meta   pol_pm[NBUF];
static struct pol_global pol_pg;

// EVOLVE-BLOCK-START
// Policy hooks (evolvable).
// LRU seed: move a buffer to the MRU end when released, evict from the LRU end.
static void
lru_unlink(int i)
{
  int nw = P_GET(i, newer), ol = P_GET(i, older);

  if(nw >= 0) P_SET(nw, older, ol); else G_SET(head, ol);
  if(ol >= 0) P_SET(ol, newer, nw); else G_SET(tail, nw);
}

static void
lru_push_head(int i)
{
  int h = G_GET(head);

  P_SET(i, newer, -1);
  P_SET(i, older, h);
  if(h >= 0) P_SET(h, newer, i); else G_SET(tail, i);
  G_SET(head, i);
}

static void
pol_init(void)
{
  int i;

  // same canonical cold-start order as xv6: buffer 0 is the first victim
  for(i = 0; i < NBUF; i++){
    P_SET(i, newer, i + 1 < NBUF ? i + 1 : -1);
    P_SET(i, older, i - 1);
  }
  G_SET(head, NBUF - 1);
  G_SET(tail, 0);
}

static void
pol_on_hit(int i)
{
  (void)i;
}

static void
pol_on_load(int i, uint blockno, uint old)
{
  (void)i; (void)blockno; (void)old;
}

static void
pol_on_release(int i)
{
  lru_unlink(i);
  lru_push_head(i);
}

static int
pol_victim(uint blockno)
{
  int i;

  (void)blockno;
  for(i = G_GET(tail); i >= 0; i = P_GET(i, newer))
    if(pol_cand_ok(i))
      return i;
  return 0;   // nothing evictable: the framework will report it
}
// EVOLVE-BLOCK-END
