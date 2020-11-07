// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

// for stealing free page from other cpu
struct spinlock global_lock;

void
kinit()
{
  for (int i = 0; i < NCPU; ++i) {
    initlock(&kmem[i].lock, "kmem");
  }

  initlock(&global_lock, "kmem_global_lock");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);

  int hart = cpuid();
  acquire(&kmem[hart].lock);
  struct run *r;
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    memset(p, 1, PGSIZE);
    r = (struct run*)p;
    r->next = kmem[hart].freelist;
    kmem[hart].freelist = r;
  }

  release(&kmem[hart].lock);
}

void *
steal(int hart)
{
  // acquire(&global_lock);
  for (int start = (hart + 1);; ++start) {
    int i = start % NCPU;
    if (i == hart) {
      // release(&global_lock);
      return 0;
    }

    acquire(&kmem[i].lock);
    struct run *r = kmem[i].freelist;
    if (r) {
      kmem[i].freelist = r->next;
      r->next = 0;
      release(&kmem[i].lock);
      // release(&global_lock);
      return (void*)r;
    }
    release(&kmem[i].lock);
  }
  // release(&global_lock);

  return 0;
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int hart = cpuid();
  pop_off();

  acquire(&kmem[hart].lock);
  r->next = kmem[hart].freelist;
  kmem[hart].freelist = r;
  release(&kmem[hart].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int hart = cpuid();
  pop_off();

  acquire(&kmem[hart].lock);
  r = kmem[hart].freelist;
  if (!r) {
    release(&kmem[hart].lock);
    r = steal(hart);
    acquire(&kmem[hart].lock);
  }

  if(r)
    kmem[hart].freelist = r->next;
  release(&kmem[hart].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
