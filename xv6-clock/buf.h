struct buf {
  int flags;
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  struct buf *qnext; // disk queue
  struct buf *hnext; // lookup hash chain (only with BCACHE_LOOKUP_HASH)
  int hashed;        // 1 if linked into a hash bucket
  uchar data[BSIZE];
};
#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk
#define B_REF   0x8  // Clock policy: recently referenced

