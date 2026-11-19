// Host stand-ins for the kernel services bio.c calls.
//   locks      -> no-ops (single thread)
//   iderw      -> the "disk": a read marks the block valid, a write clears dirty
//   ticks      -> advances on every tickslock acquire so bench_ticks() loops end
//   cprintf    -> printf
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

void cprintf(char *fmt, ...){ va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void panic(char *s){ fprintf(stderr, "panic: %s\n", s); exit(2); }
void acquire(struct spinlock *l){ if(l == &tickslock) ticks++; }
void release(struct spinlock *l){ (void)l; }
void initlock(struct spinlock *l, char *n){ (void)l; (void)n; }
void initsleeplock(struct sleeplock *l, char *n){ (void)l; (void)n; }
void acquiresleep(struct sleeplock *l){ l->locked = 1; }
void releasesleep(struct sleeplock *l){ l->locked = 0; }
int  holdingsleep(struct sleeplock *l){ return l->locked; }
void iderw(struct buf *b){
  if(b->flags & B_DIRTY){ b->flags &= ~B_DIRTY; b->flags |= B_VALID; }
  else b->flags |= B_VALID;
}
