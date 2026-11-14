// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.
//
// The implementation uses these state flags internally:
// * B_VALID: the buffer data has been read from the disk.
// * B_DIRTY: the buffer data has been modified
//     and needs to be written to disk.
// * B_REF:   Clock policy only: recently referenced.
//
// ---------------------------------------------------------------------
// Build-time options (all independent; pass via make CFLAGS_EXTRA="...")
//   (none)                        Clock, B_REF set on load, list lookup
//   -DBCACHE_LRU                  original xv6 LRU linked-list policy
//   -DBCACHE_CLOCK_NOREF_ON_LOAD  Clock variant: B_REF set only on a *re*-use
//   -DBCACHE_LOOKUP_HASH          policy-independent hash lookup (applies to
//                                 whichever policy is built; isolates the
//                                 replacement policy from lookup cost)
// ---------------------------------------------------------------------

#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

#if defined(BCACHE_LRU) && defined(BCACHE_CLOCK_NOREF_ON_LOAD)
#error "BCACHE_CLOCK_NOREF_ON_LOAD only makes sense for the Clock policy"
#endif

#ifdef BCACHE_LRU
#define BCACHE_POLICY_NAME "xv6 baseline (LRU-like; unchanged)"
#define BCACHE_POLICY_TAG  "LRU"
#define CLOCK_LOAD_FLAGS   0
#elif defined(BCACHE_CLOCK_NOREF_ON_LOAD)
#define BCACHE_POLICY_NAME "Clock (second chance; B_REF NOT set on load)"
#define BCACHE_POLICY_TAG  "CLOCK-NR"
#define CLOCK_LOAD_FLAGS   0
#else
#define BCACHE_POLICY_NAME "Clock (second chance; B_REF set on load)"
#define BCACHE_POLICY_TAG  "CLOCK"
#define CLOCK_LOAD_FLAGS   B_REF
#endif

#ifdef BCACHE_LOOKUP_HASH
#define BCACHE_LOOKUP_NAME "hash"
#else
#define BCACHE_LOOKUP_NAME "list"
#endif

#define BCACHE_NBUCKET 13        // hash buckets (only with BCACHE_LOOKUP_HASH)
#define INVALID_DEV    0xffffffff // identity of a buffer holding nothing

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  int hand;               // Clock hand: index into buf[] (Clock only)

  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.  Reordered on brelse() only for LRU.
  struct buf head;

#ifdef BCACHE_LOOKUP_HASH
  struct buf *hbucket[BCACHE_NBUCKET];
#endif
} bcache;

// ---------------------------------------------------------------------
// Benchmark configuration.
// ---------------------------------------------------------------------

// Modeled hardware costs.  IDENTICAL for every policy (see report text).
#define BENCH_US_MISS 4000   // one miss = one disk read
#define BENCH_US_HIT  10     // one hit (copy/return path)
#define BENCH_US_INSP 1      // read one cache entry (lookup node / victim candidate)
#define BENCH_US_STEP BENCH_US_INSP   // legacy name
#define BENCH_US_WRITE 1     // one store into replacement bookkeeping
                             // (list pointer, B_REF bit, Clock hand)

// Disk layout used by the workloads (FSSIZE is 1000 blocks).
#define BENCH_SCAN_START  100
#define BENCH_SCAN_REGION 400   // scan blocks live in [100,500)
#define BENCH_HOT_START   500   // hot set: [500, 500+hot)   hot <= 64
#define BENCH_WARM_START  600   // warm set (mixed): [600, 600+2*hot)
#define BENCH_TOY_START   600   // toy / safety blocks

#define BENCH_MAX_SCAN     BENCH_SCAN_REGION
#define BENCH_MAX_HOT      64
#define BENCH_MAX_REQUESTS 100000

// Workload ids (bcbench maps names to these).
#define W_TOY      0   // tiny deterministic test with hand-derivable result
#define W_SEQ      1   // one-time sequential scan
#define W_HOTSCAN  2   // original: scan, hot set revisited every `interval`
#define W_WSET     3   // repeated working set
#define W_POLLUTE  4   // hot set -> big streaming scan -> hot set
#define W_MIXED    5   // hot + scan + intermittent reuse
#define W_SAFETY   6   // pinned/dirty buffers must never be evicted
#define W_MAX      6

// Counters.  Written only by the single benchmark process (under bcache.lock
// where they are touched inside bget/brelse).  Only requests issued through
// bench_req() are counted, so other processes cannot pollute the numbers.
static struct {
  uint requests, hits, misses, disk_reads;
  uint hot_requests, hot_hits;
  uint ws_probes, ws_resident;     // working-set retention at hot-phase starts
  uint lookup_insp;                // cache entries inspected during lookup
  uint victim_searches, victim_insp;
  uint hot_evictions;              // victims that were hot-set blocks
  // LRU list maintenance
  uint lru_unlinks, lru_links, lru_ptr_writes;
  // Clock bookkeeping
  uint hand_adv, hand_writes, ref_checks, ref_clears, second_chances;
  uint ref_sets_hit, ref_sets_load;
} bench_stats;

// Per-request diagnostic record (for trace mode).
static struct {
  int hit;
  uint lookup;
  int victim, old_blk, hot_evict;
  uint insp;
  int hand0, hand1;
  uint chances;
} bench_last;

static int  bench_running;     // counting enabled
static int  bench_dry;         // dry run: generator only counts/hashes
static uint bench_hot_count;
static int  bench_trace_left;
static uint bench_req_total, bench_seq_hash;
static uint bench_scan_seq, bench_hot_phases;
static uint bench_cold_pinned;

#define BCHK(cond, msg) do { if(!(cond)){ \
  cprintf("bcache check FAILED: %s\n", msg); panic("bcache check"); } } while(0)

// ---------------------------------------------------------------------
// Hash lookup (optional)
// ---------------------------------------------------------------------
#ifdef BCACHE_LOOKUP_HASH
static uint
bhash(uint dev, uint blockno)
{
  return (dev * 31 + blockno) % BCACHE_NBUCKET;
}

static void
hash_remove(struct buf *b)
{
  struct buf **pp;

  if(!b->hashed)
    return;
  pp = &bcache.hbucket[bhash(b->dev, b->blockno)];
  while(*pp && *pp != b)
    pp = &(*pp)->hnext;
  if(*pp == 0)
    panic("bcache: hash_remove");
  *pp = b->hnext;
  b->hnext = 0;
  b->hashed = 0;
}

static void
hash_insert(struct buf *b)
{
  uint h = bhash(b->dev, b->blockno);

  b->hnext = bcache.hbucket[h];
  bcache.hbucket[h] = b;
  b->hashed = 1;
}
#endif

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

//PAGEBREAK!
  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  bcache.hand = 0;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    b->dev = INVALID_DEV;        // holds nothing (was (0,0) for all buffers)
    b->blockno = INVALID_DEV;
    b->hnext = 0;
    b->hashed = 0;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// ---------------------------------------------------------------------
// Core lookup / replacement
// ---------------------------------------------------------------------

// Find a cached block.  Caller holds bcache.lock.  `c` = count statistics.
static struct buf*
bcache_find(uint dev, uint blockno, int c)
{
  struct buf *b;
  uint steps = 0;

#ifdef BCACHE_LOOKUP_HASH
  for(b = bcache.hbucket[bhash(dev, blockno)]; b; b = b->hnext){
    steps++;
    if(b->dev == dev && b->blockno == blockno)
      break;
  }
#else
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    steps++;
    if(b->dev == dev && b->blockno == blockno)
      break;
  }
  if(b == &bcache.head)
    b = 0;
#endif
  if(c){
    bench_stats.lookup_insp += steps;
    bench_last.lookup = steps;
  }
  return b;
}

// Give victim b a new identity.  Caller holds bcache.lock.
static void
bcache_install(struct buf *b, uint dev, uint blockno, int flags, int c)
{
  // Independent safety assertions: never recycle busy or dirty buffers.
  if(b->refcnt != 0)
    panic("bcache: evicting busy buffer");
  if(b->flags & B_DIRTY)
    panic("bcache: evicting dirty buffer");

  if(c){
    bench_last.victim = b - bcache.buf;
    if(b->dev != INVALID_DEV)
      bench_last.old_blk = (int)b->blockno;
    if(b->dev == ROOTDEV && b->blockno >= BENCH_HOT_START &&
       b->blockno < BENCH_HOT_START + bench_hot_count){
      bench_stats.hot_evictions++;
      bench_last.hot_evict = 1;
    }
    if(flags & B_REF)
      bench_stats.ref_sets_load++;
  }
#ifdef BCACHE_LOOKUP_HASH
  hash_remove(b);
#endif
  b->dev = dev;
  b->blockno = blockno;
  b->flags = flags;
  b->refcnt = 1;
#ifdef BCACHE_LOOKUP_HASH
  hash_insert(b);
#endif
}

// Not cached: pick and claim a victim.  Caller holds bcache.lock.
static struct buf*
bcache_victim(uint dev, uint blockno, int c)
{
  struct buf *b;
  uint insp = 0;

#ifdef BCACHE_LRU
  // Original xv6: scan from the LRU end.
  // Even if refcnt==0, B_DIRTY indicates a buffer is in use
  // because log.c has modified it but not yet committed it.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    insp++;
    if(b->refcnt == 0 && (b->flags & B_DIRTY) == 0) {
      if(c){
        bench_stats.victim_searches++;
        bench_stats.victim_insp += insp;
        bench_last.insp = insp;      // == position from LRU end (1 = tail)
      }
      bcache_install(b, dev, blockno, 0, c);
      return b;
    }
  }
#else
  // Clock (second chance).  The hand sweeps buf[] circularly.  Busy or dirty
  // buffers are skipped (log.c pins dirty buffers).  A free buffer with B_REF
  // set has it cleared and is passed over; the first free buffer with B_REF
  // clear is the victim.  Two sweeps always suffice if anything is evictable.
  {
    int h = bcache.hand, h0 = h, n;
    uint chances = 0;

    for(n = 0; n < 2*NBUF; n++){
      b = &bcache.buf[h];
      h = (h + 1) % NBUF;
      insp++;
      if(b->refcnt != 0 || (b->flags & B_DIRTY) != 0)
        continue;
      if(c)
        bench_stats.ref_checks++;
      if(b->flags & B_REF){
        b->flags &= ~B_REF;           // second chance
        chances++;
        if(c)
          bench_stats.ref_clears++;
        continue;
      }
      bcache.hand = h;                // hand moves past the victim
      if(c){
        bench_stats.victim_searches++;
        bench_stats.victim_insp += insp;
        bench_stats.hand_adv += insp;
        bench_stats.hand_writes++;
        bench_stats.second_chances += chances;
        bench_last.insp = insp;
        bench_last.hand0 = h0;
        bench_last.hand1 = h;
        bench_last.chances = chances;
      }
      bcache_install(b, dev, blockno, CLOCK_LOAD_FLAGS, c);
      return b;
    }
  }
#endif
  panic("bget: no buffers");
  return 0;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// count != 0 marks a request issued by the benchmark driver.
static struct buf*
bget_ex(uint dev, uint blockno, int count)
{
  struct buf *b;
  int c;

  acquire(&bcache.lock);
  c = count && bench_running;
  if(c){
    bench_stats.requests++;
    bench_last.hit = 0;
    bench_last.lookup = 0;
    bench_last.victim = -1;
    bench_last.old_blk = -1;
    bench_last.hot_evict = 0;
    bench_last.insp = 0;
    bench_last.hand0 = -1;
    bench_last.hand1 = -1;
    bench_last.chances = 0;
  }

  // Is the block already cached?
  b = bcache_find(dev, blockno, c);
  if(b){
#ifndef BCACHE_LRU
    b->flags |= B_REF;              // Clock: mark as recently used
    if(c)
      bench_stats.ref_sets_hit++;
#endif
    if(c){
      bench_stats.hits++;
      bench_last.hit = 1;
    }
    b->refcnt++;
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }

  // Not cached; recycle an unused buffer.
  if(c)
    bench_stats.misses++;
  b = bcache_victim(dev, blockno, c);
  release(&bcache.lock);
  acquiresleep(&b->lock);
  return b;
}

static struct buf*
bread_ex(uint dev, uint blockno, int count)
{
  struct buf *b;
  int valid;

  b = bget_ex(dev, blockno, count);
  valid = (b->flags & B_VALID) != 0;

  if(count && bench_running){
    // hit/miss recorded by bget must agree with the B_VALID flag
    BCHK(valid == bench_last.hit, "hit/miss accounting mismatch");
    if(!valid)
      bench_stats.disk_reads++;
  }

  // An invalid buffer needs to be read from disk.
  if(!valid)
    iderw(b);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  return bread_ex(dev, blockno, 0);
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  b->flags |= B_DIRTY;
  iderw(b);
}

static void
brelse_ex(struct buf *b, int count)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
#ifdef BCACHE_LRU
  if (b->refcnt == 0) {
    // no one is waiting for it: move to MRU position.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
    if(count && bench_running){
      bench_stats.lru_unlinks++;
      bench_stats.lru_links++;
      bench_stats.lru_ptr_writes += 6;   // 2 for unlink + 4 for link
    }
  }
#else
  (void)count;                          // Clock: nothing to maintain on release
#endif
  release(&bcache.lock);
}

// Release a locked buffer.
// LRU: move to the head of the MRU list.  Clock: no reordering.
void
brelse(struct buf *b)
{
  brelse_ex(b, 0);
}

// ---------------------------------------------------------------------
// Invariant checks (benchmark only; they touch no counters)
// ---------------------------------------------------------------------
static int
inbuf(struct buf *p)
{
  return p >= bcache.buf && p < bcache.buf + NBUF;
}

static void
bcache_check(void)
{
  struct buf *b, *c2;
  int n, hashed = 0, valid_ids = 0;

  acquire(&bcache.lock);

#ifndef BCACHE_LRU
  BCHK(bcache.hand >= 0 && bcache.hand < NBUF, "Clock hand out of range");
#endif

  // doubly-linked list integrity (valid for both policies)
  n = 0;
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    BCHK(inbuf(b), "list node outside buf[]");
    BCHK(b->prev->next == b, "list prev/next mismatch");
    n++;
    BCHK(n <= NBUF, "list cycle or too long");
  }
  BCHK(n == NBUF, "list does not contain every buffer exactly once");
  BCHK(bcache.head.prev->next == &bcache.head, "list tail not linked to head");

  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    BCHK(b->refcnt <= NPROC, "refcnt out of range");
#ifdef BCACHE_LRU
    BCHK((b->flags & B_REF) == 0, "B_REF set under LRU");
#endif
    if(b->dev != INVALID_DEV){
      valid_ids++;
      for(c2 = b + 1; c2 < bcache.buf + NBUF; c2++)
        BCHK(!(c2->dev == b->dev && c2->blockno == b->blockno),
             "duplicate cached block identity");
    }
#ifdef BCACHE_LOOKUP_HASH
    if(b->hashed)
      hashed++;
    BCHK(b->hashed == (b->dev != INVALID_DEV), "hash membership mismatch");
#endif
  }
#ifdef BCACHE_LOOKUP_HASH
  {
    int i, cnt = 0;
    for(i = 0; i < BCACHE_NBUCKET; i++)
      for(b = bcache.hbucket[i]; b; b = b->hnext){
        BCHK(inbuf(b), "hash node outside buf[]");
        BCHK(bhash(b->dev, b->blockno) == (uint)i, "buffer in wrong bucket");
        cnt++;
        BCHK(cnt <= NBUF, "hash chain cycle");
      }
    BCHK(cnt == hashed && hashed == valid_ids, "hash chain membership");
  }
#else
  (void)hashed;
  (void)valid_ids;
#endif
  release(&bcache.lock);
}

// Side-effect-free residency probe (no counters, no B_REF, no reordering).
static int
bench_peek(uint blk, int *refcnt, int *flags)
{
  struct buf *b;
  int found = 0;

  acquire(&bcache.lock);
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    if(b->dev == ROOTDEV && b->blockno == blk){
      found = 1;
      if(refcnt) *refcnt = b->refcnt;
      if(flags) *flags = b->flags;
      break;
    }
  }
  release(&bcache.lock);
  return found;
}

// ---------------------------------------------------------------------
// Workload generator.  The request sequence depends ONLY on the workload
// parameters, never on cache state, so both policies see the same sequence.
// ---------------------------------------------------------------------
static uint bench_scan_blocks, bench_hot_interval, bench_hot_passes, bench_rounds_g;

static void
bench_req(uint blk, int hot)
{
  struct buf *b;

  bench_req_total++;
  bench_seq_hash = (bench_seq_hash ^ blk) * 16777619u;
  if(bench_dry)
    return;

  b = bread_ex(ROOTDEV, blk, 1);
  brelse_ex(b, 1);
  if(hot){
    bench_stats.hot_requests++;
    if(bench_last.hit)
      bench_stats.hot_hits++;
  }
  bcache_check();

  if(bench_trace_left > 0){
    bench_trace_left--;
    cprintf("T%d blk=%d%s %s lk=%d", bench_req_total, blk, hot ? "(H)" : "",
            bench_last.hit ? "HIT " : "MISS", bench_last.lookup);
    if(!bench_last.hit){
      cprintf(" victim=buf%d old=%d insp=%d", bench_last.victim,
              bench_last.old_blk, bench_last.insp);
#ifdef BCACHE_LRU
      cprintf(" lrupos=%d", bench_last.insp);
#else
      cprintf(" hand=%d->%d chances=%d", bench_last.hand0, bench_last.hand1,
              bench_last.chances);
#endif
      if(bench_last.hot_evict)
        cprintf(" HOT-EVICT");
    }
    cprintf("\n");
  }
}

static void
bench_hot_phase(uint passes)
{
  uint p, j, n = 0;

  // Retention probe: how many hot blocks survived since the last phase?
  // Skipped for the very first phase (cold cache by construction).
  if(!bench_dry && bench_hot_phases > 0){
    for(j = 0; j < bench_hot_count; j++)
      if(bench_peek(BENCH_HOT_START + j, 0, 0))
        n++;
    bench_stats.ws_probes += bench_hot_count;
    bench_stats.ws_resident += n;
    if(bench_trace_left > 0)
      cprintf("-- hot phase: %d/%d hot blocks resident\n", n, bench_hot_count);
  }
  bench_hot_phases++;
  for(p = 0; p < passes; p++)
    for(j = 0; j < bench_hot_count; j++)
      bench_req(BENCH_HOT_START + j, 1);
}

static uint
bench_next_scan_blk(void)
{
  return BENCH_SCAN_START + (bench_scan_seq++ % BENCH_SCAN_REGION);
}

// toy-test observations (filled in live mode)
static int  toy_r0, toy_r1;
static uint toy_insp, toy_chances;

static void
gen_toy(void)
{
  uint i, T = BENCH_TOY_START, insp0, ch0;

  for(i = 0; i < NBUF; i++)          // 1. fill the cache with T..T+NBUF-1
    bench_req(T + i, 0);
  bench_req(T, 0);                   // 2. re-reference T (hit)
  insp0 = bench_stats.victim_insp;
  ch0 = bench_stats.second_chances;
  bench_req(T + NBUF, 0);            // 3. one miss: somebody is evicted
  if(!bench_dry){
    toy_insp = bench_stats.victim_insp - insp0;
    toy_chances = bench_stats.second_chances - ch0;
    toy_r0 = bench_peek(T, 0, 0);
    toy_r1 = bench_peek(T + 1, 0, 0);
  }
  bench_req(T, 0);                   // 4. is T still cached?
}

static void
gen_safety(void)
{
  uint i, T = BENCH_TOY_START;
  struct buf *pb = 0, *db;
  int rc, fl;

  if(!bench_dry){
    pb = bread(ROOTDEV, T);          // pinned: held for the whole flood
    db = bread(ROOTDEV, T + 1);      // dirty, released
    db->flags |= B_DIRTY;
    brelse(db);
  }
  for(i = 0; i < 2*NBUF + 5; i++)    // flood: every other buffer recycled
    bench_req(T + 2 + i, 0);
  if(!bench_dry){
    BCHK(bench_peek(T, &rc, &fl) && rc == 1, "pinned buffer was evicted");
    BCHK(bench_peek(T + 1, &rc, &fl) && rc == 0 && (fl & B_DIRTY),
         "dirty buffer was evicted");
    brelse(pb);
    acquire(&bcache.lock);           // un-dirty it (data was never modified)
    for(pb = bcache.buf; pb < bcache.buf + NBUF; pb++)
      if(pb->dev == ROOTDEV && pb->blockno == T + 1)
        pb->flags &= ~B_DIRTY;
    release(&bcache.lock);
  }
}

static void
generate(int w)
{
  uint r, i, j, base, reuse;

  switch(w){
  case W_TOY:
    gen_toy();
    break;
  case W_SEQ:                         // never-reused blocks
    for(r = 0; r < bench_rounds_g; r++)
      for(i = 0; i < bench_scan_blocks; i++)
        bench_req(bench_next_scan_blk(), 0);
    break;
  case W_HOTSCAN:                     // original workload, same blocks each round
    for(r = 0; r < bench_rounds_g; r++)
      for(i = 0; i < bench_scan_blocks; i++){
        bench_req(BENCH_SCAN_START + i, 0);
        if((i + 1) % bench_hot_interval == 0)
          bench_hot_phase(bench_hot_passes);
      }
    break;
  case W_WSET:
    for(r = 0; r < bench_rounds_g; r++)
      bench_hot_phase(bench_hot_passes);
    break;
  case W_POLLUTE:                     // hot, scan, hot, scan, ..., hot
    for(r = 0; r < bench_rounds_g; r++){
      bench_hot_phase(bench_hot_passes);
      for(i = 0; i < bench_scan_blocks; i++)
        bench_req(bench_next_scan_blk(), 0);
    }
    bench_hot_phase(bench_hot_passes);
    break;
  case W_MIXED:
    for(r = 0; r < bench_rounds_g; r++){
      bench_hot_phase(bench_hot_passes);
      base = bench_scan_seq;
      for(i = 0; i < bench_scan_blocks; i++){
        bench_req(bench_next_scan_blk(), 0);
        if((i + 1) % bench_hot_interval == 0)
          bench_hot_phase(1);
      }
      // short-distance reuse of the blocks just scanned
      reuse = bench_hot_count < bench_scan_blocks ? bench_hot_count
                                                  : bench_scan_blocks;
      for(j = 0; j < reuse; j++)
        bench_req(BENCH_SCAN_START +
                  ((base + bench_scan_blocks - reuse + j) % BENCH_SCAN_REGION), 0);
      // intermittent reuse: warm set read once per round
      for(j = 0; j < 2 * bench_hot_count; j++)
        bench_req(BENCH_WARM_START + j, 0);
    }
    break;
  case W_SAFETY:
    gen_safety();
    break;
  }
}

// ---------------------------------------------------------------------
// Benchmark driver
// ---------------------------------------------------------------------
static uint
bench_ticks(void)
{
  uint t;
  acquire(&tickslock);
  t = ticks;
  release(&tickslock);
  return t;
}

// Identical cold start for every policy: every free buffer loses its identity
// and flags (so B_VALID and B_REF are clear), is re-linked in canonical order
// (buf[0] ends up as the LRU tail), and the Clock hand is set to buf[0].
// Hence LRU's first victim and Clock's first victim are the same buffer, and
// the first NBUF misses fill the cache in the same order under both policies.
static void
bench_cold_start(void)
{
  struct buf *b;

  acquire(&bcache.lock);
  bench_running = 0;
  bench_cold_pinned = 0;
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    if(b->refcnt == 0 && (b->flags & B_DIRTY) == 0){
#ifdef BCACHE_LOOKUP_HASH
      hash_remove(b);
#endif
      b->flags = 0;
      b->dev = INVALID_DEV;
      b->blockno = INVALID_DEV;
      b->next->prev = b->prev;
      b->prev->next = b->next;
      b->next = bcache.head.next;
      b->prev = &bcache.head;
      bcache.head.next->prev = b;
      bcache.head.next = b;
    } else
      bench_cold_pinned++;
  }
  bcache.hand = 0;
  {
    uint *p = (uint*)&bench_stats;
    uint k;
    for(k = 0; k < sizeof(bench_stats)/sizeof(uint); k++)
      p[k] = 0;
  }
  release(&bcache.lock);
}

// x/y as a fixed-point value with `scale` (10 or 100) decimals, no overflow.
static uint
ratio(uint x, uint y, uint scale)
{
  if(y == 0)
    return 0;
  return (x / y) * scale + ((x % y) * scale) / y;
}

static void
print_pct(char *label, uint num, uint den)
{
  uint t = ratio(num, den, 1000);
  cprintf("%s: %d.%d%%\n", label, t / 10, t % 10);
}

static void
print_avg(char *label, uint num, uint den)
{
  uint t = ratio(num, den, 100);
  cprintf("%s: %d.%d%d\n", label, t / 100, (t / 10) % 10, t % 10);
}

static void
toy_verify(void)
{
  int e_r0, e_r1;
  uint e_insp, e_chances, e_hits;

#ifdef BCACHE_LRU
  e_r0 = 1; e_r1 = 0; e_insp = 1; e_chances = 0; e_hits = 2;
#elif defined(BCACHE_CLOCK_NOREF_ON_LOAD)
  e_r0 = 1; e_r1 = 0; e_insp = 2; e_chances = 1; e_hits = 2;
#else
  e_r0 = 0; e_r1 = 1; e_insp = NBUF + 1; e_chances = NBUF; e_hits = 1;
#endif
  if(bench_cold_pinned){
    cprintf("TOY: skipped (pinned/dirty buffers at start)\n");
    return;
  }
  cprintf("TOY: T resident=%d (expect %d), T+1 resident=%d (expect %d)\n",
          toy_r0, e_r0, toy_r1, e_r1);
  cprintf("TOY: victim search inspected %d (expect %d), second chances %d (expect %d)\n",
          toy_insp, e_insp, toy_chances, e_chances);
  cprintf("TOY: hits %d (expect %d)\n", bench_stats.hits, e_hits);
  BCHK(toy_r0 == e_r0 && toy_r1 == e_r1, "toy: wrong victim");
  BCHK(toy_insp == e_insp, "toy: wrong inspection count");
  BCHK(toy_chances == e_chances, "toy: wrong second-chance count");
  BCHK(bench_stats.hits == e_hits, "toy: wrong hit count");
  cprintf("TOY CHECK: PASS\n");
}

// Returns 0 on success, -1 on invalid parameters.
int
bcache_bench(int workload, int scan_blocks, int hot_count, int hot_interval,
             int hot_passes, int rounds, int trace)
{
  uint start_ticks, end_ticks, wall_ticks, delay_us, ticks_model;
  uint exp_req, exp_hash;
  uint io_us, hit_us, lookup_us, repl_us, repl_writes;
  struct buf *b;

  if(workload < 0 || workload > W_MAX ||
     scan_blocks < 1 || scan_blocks > BENCH_MAX_SCAN ||
     hot_count < 1 || hot_count > BENCH_MAX_HOT ||
     hot_interval < 1 || hot_interval > BENCH_MAX_SCAN ||
     hot_passes < 1 || hot_passes > 20 ||
     rounds < 1 || rounds > 100 || trace < 0 || trace > 5000 ||
     NBUF < 3)
    return -1;
  if((workload == W_TOY || workload == W_SAFETY) &&
     BENCH_TOY_START + 2*NBUF + 8 >= FSSIZE)
    return -1;

  bench_scan_blocks = scan_blocks;
  bench_hot_count = hot_count;
  bench_hot_interval = hot_interval;
  bench_hot_passes = hot_passes;
  bench_rounds_g = rounds;

  // Dry run: the SAME generator, no cache access.  Gives the expected request
  // count and sequence checksum (policy independent by construction).
  bench_dry = 1;
  bench_req_total = 0; bench_seq_hash = 2166136261u;
  bench_scan_seq = 0; bench_hot_phases = 0;
  generate(workload);
  exp_req = bench_req_total;
  exp_hash = bench_seq_hash;
  bench_dry = 0;
  if(exp_req > BENCH_MAX_REQUESTS || exp_req == 0)
    return -1;

  bench_req_total = 0; bench_seq_hash = 2166136261u;
  bench_scan_seq = 0; bench_hot_phases = 0;
  bench_cold_start();
  bcache_check();

  // Tick-align the start (informational wall-clock only).
  start_ticks = bench_ticks();
  while(bench_ticks() == start_ticks)
    ;
  start_ticks = bench_ticks();

  bench_trace_left = trace;
  if(trace)
    cprintf("--- trace (first %d requests; H = hot-set block) ---\n", trace);
  acquire(&bcache.lock);
  bench_running = 1;
  release(&bcache.lock);

  generate(workload);

  acquire(&bcache.lock);
  bench_running = 0;
  release(&bcache.lock);
  end_ticks = bench_ticks();
  wall_ticks = end_ticks - start_ticks;
  bench_trace_left = 0;
  bcache_check();

  // ---- consistency checks on the counters ----
  BCHK(bench_stats.hits + bench_stats.misses == bench_stats.requests,
       "hits + misses != requests");
  BCHK(bench_stats.requests == bench_req_total, "cache and driver disagree on requests");
  BCHK(bench_req_total == exp_req && bench_seq_hash == exp_hash,
       "live request sequence differs from generator dry run");
  BCHK(bench_stats.disk_reads == bench_stats.misses, "disk_reads != misses");
  BCHK(bench_stats.victim_searches == bench_stats.misses, "victim searches != misses");
  BCHK(bench_stats.victim_insp >= bench_stats.victim_searches, "victim_insp too small");
  BCHK(bench_stats.hot_hits <= bench_stats.hot_requests, "hot hits > hot requests");
  BCHK(bench_stats.ws_resident <= bench_stats.ws_probes, "retention counters");
#ifdef BCACHE_LRU
  BCHK(bench_stats.lru_unlinks == bench_stats.lru_links &&
       bench_stats.lru_links == bench_stats.requests &&
       bench_stats.lru_ptr_writes == 6 * bench_stats.lru_links, "LRU list-op counters");
  BCHK(bench_stats.hand_adv == 0 && bench_stats.ref_checks == 0 &&
       bench_stats.ref_clears == 0 && bench_stats.ref_sets_hit == 0 &&
       bench_stats.ref_sets_load == 0, "Clock counters nonzero under LRU");
#else
  BCHK(bench_stats.second_chances == bench_stats.ref_clears, "second chances != ref clears");
  BCHK(bench_stats.ref_clears <= bench_stats.ref_checks, "ref clears > ref checks");
  BCHK(bench_stats.hand_adv == bench_stats.victim_insp, "hand advances != victim inspections");
  BCHK(bench_stats.hand_writes == bench_stats.victim_searches, "hand writes != searches");
  BCHK(bench_stats.ref_sets_hit == bench_stats.hits, "ref sets != hits");
  BCHK(bench_stats.lru_unlinks == 0 && bench_stats.lru_links == 0 &&
       bench_stats.lru_ptr_writes == 0, "LRU counters nonzero under Clock");
#endif

  // ---- modeled cost: every component reported, same unit costs everywhere ----
#ifdef BCACHE_LRU
  repl_writes = bench_stats.lru_ptr_writes;
#else
  repl_writes = bench_stats.ref_clears + bench_stats.ref_sets_hit +
                bench_stats.ref_sets_load + bench_stats.hand_writes;
#endif
  io_us     = bench_stats.misses * BENCH_US_MISS;
  hit_us    = bench_stats.hits * BENCH_US_HIT;
  lookup_us = bench_stats.lookup_insp * BENCH_US_INSP;
  repl_us   = bench_stats.victim_insp * BENCH_US_INSP + repl_writes * BENCH_US_WRITE;
  delay_us  = io_us + hit_us + lookup_us + repl_us;
  ticks_model = delay_us / 10000;

  (void)b;
  cprintf("\n=== xv6 Buffer Cache Benchmark ===\n");
  cprintf("Policy: %s\n", BCACHE_POLICY_NAME);
  cprintf("Lookup structure: %s\n", BCACHE_LOOKUP_NAME);
  cprintf("Cache capacity: %d buffers\n", NBUF);
  cprintf("Buffer structure size: %d bytes\n", (int)sizeof(struct buf));
  cprintf("Total Mem (buffer cache): %d bytes\n", (int)sizeof(bcache.buf));
  cprintf("Workload id: %d  scan=%d hot=%d interval=%d passes=%d rounds=%d\n",
          workload, scan_blocks, hot_count, hot_interval, hot_passes, rounds);
  cprintf("Cold start: pinned/dirty buffers not reset: %d\n", bench_cold_pinned);
  cprintf("Request sequence checksum: %x\n", bench_seq_hash);
  cprintf("Cost model (us): miss=%d hit=%d inspect=%d write=%d\n",
          BENCH_US_MISS, BENCH_US_HIT, BENCH_US_INSP, BENCH_US_WRITE);

  cprintf("-- 1. Cache effectiveness --\n");
  cprintf("Requests: %d\n", bench_stats.requests);
  cprintf("Cache hits: %d\n", bench_stats.hits);
  cprintf("Cache misses: %d\n", bench_stats.misses);
  print_pct("Hit rate", bench_stats.hits, bench_stats.requests);
  cprintf("Hot-set requests: %d\n", bench_stats.hot_requests);
  cprintf("Hot-set hits: %d\n", bench_stats.hot_hits);
  print_pct("Hot-set hit rate", bench_stats.hot_hits, bench_stats.hot_requests);
  cprintf("Hot-set blocks evicted: %d\n", bench_stats.hot_evictions);
  cprintf("Working-set probes (hot blocks checked at phase start): %d\n", bench_stats.ws_probes);
  cprintf("Working-set blocks still resident: %d\n", bench_stats.ws_resident);
  print_pct("Working-set retention", bench_stats.ws_resident, bench_stats.ws_probes);

  cprintf("-- 2. Lookup cost --\n");
  cprintf("Lookup inspections: %d\n", bench_stats.lookup_insp);
  print_avg("Lookup inspections per request", bench_stats.lookup_insp, bench_stats.requests);

  cprintf("-- 3. Replacement cost --\n");
  cprintf("Victim searches: %d\n", bench_stats.victim_searches);
  cprintf("Victim inspections: %d\n", bench_stats.victim_insp);
  print_avg("Victim inspections per miss", bench_stats.victim_insp, bench_stats.victim_searches);
  cprintf("LRU list unlinks: %d\n", bench_stats.lru_unlinks);
  cprintf("LRU list links: %d\n", bench_stats.lru_links);
  cprintf("LRU pointer writes: %d\n", bench_stats.lru_ptr_writes);
  cprintf("Clock hand advances: %d\n", bench_stats.hand_adv);
  cprintf("Clock reference-bit checks: %d\n", bench_stats.ref_checks);
  cprintf("Clock reference-bit clears: %d\n", bench_stats.ref_clears);
  cprintf("Clock second-chance events: %d\n", bench_stats.second_chances);
  cprintf("Clock reference-bit sets (hit): %d\n", bench_stats.ref_sets_hit);
  cprintf("Clock reference-bit sets (load): %d\n", bench_stats.ref_sets_load);
  cprintf("Clock hand writes: %d\n", bench_stats.hand_writes);
  cprintf("Replacement bookkeeping writes: %d\n", repl_writes);

  cprintf("-- 4. Modeled total cost (us) --\n");
  cprintf("I/O cost (misses): %d\n", io_us);
  cprintf("Hit cost: %d\n", hit_us);
  cprintf("Lookup cost: %d\n", lookup_us);
  cprintf("Replacement cost: %d\n", repl_us);
  cprintf("Management cost (lookup+replacement): %d\n", lookup_us + repl_us);
  cprintf("Total modeled cost: %d\n", delay_us);
  cprintf("Delay: %d timer ticks (deterministic model)\n", ticks_model);
  cprintf("Wall-clock ticks (informational, varies): %d\n", wall_ticks);

  cprintf("CSVH,policy,lookup,nbuf,workload,scan,hot,interval,passes,rounds,requests,hits,misses,hot_req,hot_hits,ws_probes,ws_res,lookup_insp,vic_searches,vic_insp,lru_unlink,lru_link,lru_ptrw,hand_adv,ref_checks,ref_clears,second_chances,ref_set_hit,ref_set_load,hand_writes,hot_evict,io_us,hit_us,lookup_us,repl_us,total_us,seqhash\n");
  cprintf("CSV,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%x\n",
          BCACHE_POLICY_TAG, BCACHE_LOOKUP_NAME, NBUF, workload, scan_blocks,
          hot_count, hot_interval, hot_passes, rounds,
          bench_stats.requests, bench_stats.hits, bench_stats.misses,
          bench_stats.hot_requests, bench_stats.hot_hits,
          bench_stats.ws_probes, bench_stats.ws_resident,
          bench_stats.lookup_insp, bench_stats.victim_searches, bench_stats.victim_insp,
          bench_stats.lru_unlinks, bench_stats.lru_links, bench_stats.lru_ptr_writes,
          bench_stats.hand_adv, bench_stats.ref_checks, bench_stats.ref_clears,
          bench_stats.second_chances, bench_stats.ref_sets_hit, bench_stats.ref_sets_load,
          bench_stats.hand_writes, bench_stats.hot_evictions,
          io_us, hit_us, lookup_us, repl_us, delay_us, bench_seq_hash);

  if(workload == W_TOY)
    toy_verify();
  cprintf("Verification: all invariants and counter checks PASSED\n");
  cprintf("==================================\n");
  return 0;
}
