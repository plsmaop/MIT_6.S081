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


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
#define DEBUG 0
struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

struct spinlock locks[NBUCKET];

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for (int i = 0; i < NBUCKET; ++i) {
    initlock(&locks[i], "bcache");
  }

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for (b = bcache.buf; b < bcache.buf+NBUF; b++) {
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    b->shard = -1;
    b->ticks = 0;
    b->refcnt = 0;
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // acquire(&bcache.lock);

  int shard = blockno % NBUCKET;
#if DEBUG  
  printf("try acquire: %d\n", shard);
#endif
  acquire(&locks[shard]);

  // Is the block already cached ?
  for (b = bcache.buf; b < bcache.buf+NBUF; b++) {
    if (b->shard == shard && b->dev == dev && b->blockno == blockno) {
      b->refcnt++;

      acquire(&tickslock);
      b->ticks = ticks;
      release(&tickslock);

      release(&locks[shard]);
#if DEBUG  
      printf("get buf, release %d\n", shard);
#endif
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&locks[shard]);
#if DEBUG  
  printf("get no buf, release %d\n", shard);
#endif

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
#if DEBUG 
  printf("try acquire: bcache lock\n");
#endif
  acquire(&bcache.lock);
  acquire(&tickslock);
  int cur_ticks = ticks;
  release(&tickslock);

  int min_ticks = cur_ticks + 9487;
  int buf_ind = -1;

  for (int i = 0; i < NBUF; ++i) {
    b = &bcache.buf[i];
    if (b->refcnt == 0 && b->ticks < min_ticks) {
      min_ticks = b->ticks;
      buf_ind = i;
    }
  }

  if (buf_ind == -1) {
    panic("bget: no buffers");
  }

#if DEBUG 
  printf("try acquire: %d for new buf\n", shard);
#endif
  acquire(&locks[shard]);

  b = &bcache.buf[buf_ind];
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  b->ticks = cur_ticks;
  b->shard = shard;

  release(&locks[shard]);
#if DEBUG 
  printf("modified new buf, release %d\n", shard);
#endif
  release(&bcache.lock);
#if DEBUG 
  printf("release bcache lock\n");
#endif
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int shard = b->shard;
#if DEBUG 
  printf("try acquire %d for brelease\n", shard);
#endif
  acquire(&locks[shard]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->shard = -1;
    b->blockno = -1;
    b->dev = -1;
  }
  
  release(&locks[shard]);
#if DEBUG 
  printf("finish brelease, release %d\n", shard);
#endif
}

void
bpin(struct buf *b) {
  int shard = b->shard;

  acquire(&locks[shard]);
  b->refcnt++;
  release(&locks[shard]);
}

void
bunpin(struct buf *b) {
  int shard = b->shard;

  acquire(&locks[shard]);
  b->refcnt--;
  release(&locks[shard]);
}


