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
// The implementation uses two state flags internally:
// * B_VALID: the buffer data has been read from the disk.
// * B_DIRTY: the buffer data has been modified
//     and needs to be written to disk.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.
  struct buf head;
} bcache;
// Benchmark configuration.
#define BENCH_HOT_START     500
#define BENCH_HOT_COUNT     4
#define BENCH_SCAN_START    100
#define BENCH_SCAN_BLOCKS   120
#define BENCH_SCAN_INTERVAL 40
#define BENCH_HOT_PASSES    3
#define BENCH_ROUNDS        5
#define BENCH_MAX_SCAN_BLOCKS 300

// Benchmark statistics.
static int bench_active;

static struct {
  uint requests;
  uint hits;
  uint misses;
  uint disk_reads;
  uint hot_requests;
  uint hot_hits;
  uint lookup_steps;
  uint victim_steps;
} bench_stats;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

//PAGEBREAK!
  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(bench_active)
      bench_stats.lookup_steps++;
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached; recycle an unused buffer.
  // Even if refcnt==0, B_DIRTY indicates a buffer is in use
  // because log.c has modified it but not yet committed it.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(bench_active)
      bench_stats.victim_steps++;
    if(b->refcnt == 0 && (b->flags & B_DIRTY) == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->flags = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.

// Write b's contents to disk.  Must be locked.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;
  int valid;

  b = bget(dev, blockno);

  // bget() returns the buffer with its sleep lock held.
  valid = (b->flags & B_VALID) != 0;

  // Record statistics only while our benchmark is running.
  acquire(&bcache.lock);

  if(bench_active) {
    bench_stats.requests++;

    if(valid) {
      bench_stats.hits++;
    } else {
      bench_stats.misses++;
      bench_stats.disk_reads++;
    }

    if(dev == ROOTDEV &&
       blockno >= BENCH_HOT_START &&
       blockno < BENCH_HOT_START + BENCH_HOT_COUNT) {
      bench_stats.hot_requests++;

      if(valid)
        bench_stats.hot_hits++;
    }
  }

  release(&bcache.lock);

  // An invalid buffer needs to be read from disk.
  if(!valid)
    iderw(b);

  return b;
}
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  b->flags |= B_DIRTY;
  iderw(b);
}

// Release a locked buffer.
// Move to the head of the MRU list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}
static uint
bench_ticks(void)
{
  uint t;
  acquire(&tickslock);
  t = ticks;
  release(&tickslock);
  return t;
}

void
bcache_bench(int scan_blocks, int rounds)
{
  struct buf *b;
  uint i, j, pass, round;
  uint requests, hits, misses, disk_reads;
  uint hot_requests, hot_hits, lookup_steps, victim_steps;
  uint start_ticks, end_ticks, elapsed_ticks;
  uint hit_rate10, hot_rate10;

  if(scan_blocks < 1 || scan_blocks > BENCH_MAX_SCAN_BLOCKS ||
     rounds < 1 || rounds > 20)
    panic("bcache_bench: invalid parameters");

  /*
   * Prepare a mostly cold cache without invalidating referenced or
   * dirty buffers. Preserve the existing LRU list and replacement rule.
   */
  acquire(&bcache.lock);
  bench_active = 0;

  for(b = bcache.buf; b < bcache.buf + NBUF; b++) {
    if(b->refcnt == 0 && (b->flags & B_DIRTY) == 0) {
      // Keep the buffer's device/block identity unique; clearing B_VALID
      // is sufficient to make its contents cold for benchmark purposes.
      b->flags = 0;
    }
  }

  bench_stats.requests = 0;
  bench_stats.hits = 0;
  bench_stats.misses = 0;
  bench_stats.disk_reads = 0;
  bench_stats.hot_requests = 0;
  bench_stats.hot_hits = 0;
  bench_stats.lookup_steps = 0;
  bench_stats.victim_steps = 0;
  release(&bcache.lock);

  start_ticks = bench_ticks();
  acquire(&bcache.lock);
  bench_active = 1;
  release(&bcache.lock);

  /*
   * Mixed workload: scan blocks starting at 100. Every 40 scan blocks,
   * revisit the four-block hot set starting at 500, three times.
   * Maximum scan length is 300 so it does not overlap the hot set.
   */
  for(round = 0; round < (uint)rounds; round++) {
    for(i = 0; i < (uint)scan_blocks; i++) {
      b = bread(ROOTDEV, BENCH_SCAN_START + i);
      brelse(b);

      if((i + 1) % BENCH_SCAN_INTERVAL == 0) {
        for(pass = 0; pass < BENCH_HOT_PASSES; pass++) {
          for(j = 0; j < BENCH_HOT_COUNT; j++) {
            b = bread(ROOTDEV, BENCH_HOT_START + j);
            brelse(b);
          }
        }
      }
    }
  }

  end_ticks = bench_ticks();
  elapsed_ticks = end_ticks - start_ticks;

  acquire(&bcache.lock);
  bench_active = 0;
  requests = bench_stats.requests;
  hits = bench_stats.hits;
  misses = bench_stats.misses;
  disk_reads = bench_stats.disk_reads;
  hot_requests = bench_stats.hot_requests;
  hot_hits = bench_stats.hot_hits;
  lookup_steps = bench_stats.lookup_steps;
  victim_steps = bench_stats.victim_steps;
  release(&bcache.lock);

  hit_rate10 = requests ? (hits * 1000) / requests : 0;
  hot_rate10 = hot_requests ? (hot_hits * 1000) / hot_requests : 0;

  cprintf("\n=== xv6 Buffer Cache Benchmark ===\n");
  cprintf("Policy: xv6 baseline (LRU-like; unchanged)\n");
  cprintf("Cache capacity: %d buffers\n", NBUF);
  cprintf("Buffer structure size: %d bytes\n", (int)sizeof(struct buf));
  cprintf("Total Mem (buffer cache): %d bytes\n", (int)sizeof(bcache.buf));
  cprintf("Workload scan blocks: %d\n", scan_blocks);
  cprintf("Workload rounds: %d\n", rounds);
  cprintf("Requests: %d\n", requests);
  cprintf("Cache hits: %d\n", hits);
  cprintf("Cache misses: %d\n", misses);
  cprintf("Disk read requests (miss-triggered estimate): %d\n", disk_reads);
  cprintf("Hit rate: %d.%d%%\n", hit_rate10 / 10, hit_rate10 % 10);
  cprintf("Hot-set requests: %d\n", hot_requests);
  cprintf("Hot-set hits: %d\n", hot_hits);
  cprintf("Hot-set hit rate: %d.%d%%\n", hot_rate10 / 10, hot_rate10 % 10);
  cprintf("Delay: %d timer ticks\n", elapsed_ticks);
  cprintf("Approx. delay: %d ms (assuming 10 ms/tick)\n", elapsed_ticks * 10);
  cprintf("Lookup list inspections: %d\n", lookup_steps);
  cprintf("Victim-selection inspections: %d\n", victim_steps);
  cprintf("Total Compute (list-inspection proxy): %d\n", lookup_steps + victim_steps);
  cprintf("==================================\n");
}
//PAGEBREAK!
// Blank page.

