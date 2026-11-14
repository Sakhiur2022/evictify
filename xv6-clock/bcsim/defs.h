// Host-side stand-in for xv6's defs.h: only what bio.c needs.
// bio.c itself is compiled UNMODIFIED against this.
#ifndef BCSIM_DEFS_H
#define BCSIM_DEFS_H
struct buf;
struct spinlock;
struct sleeplock;

void            cprintf(char*, ...);
void            panic(char*) __attribute__((noreturn));
void            acquire(struct spinlock*);
void            release(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            initsleeplock(struct sleeplock*, char*);
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            iderw(struct buf*);

extern struct spinlock tickslock;
extern uint     ticks;

// provided by bio.c
void            binit(void);
struct buf*     bread(uint, uint);
void            bwrite(struct buf*);
void            brelse(struct buf*);
int             bcache_bench(int, int, int, int, int, int, int);
#endif
