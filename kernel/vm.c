#include "param.h"
#include "types.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "proc.h"
#include "fcntl.h"
#include "file.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }

  if ((*pte & PTE_V) == 0) {
    if (PGROUNDUP(va) + PGSIZE > myproc()->sz) {
      return 0;
    }

    char *mem = kalloc();
    if (mem == 0) {
      return 0;
    } 

    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, va, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_U) != 0) {
      kfree(mem);
      return 0;
    }
  }

  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");
  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) {
      // no page exist
      continue;
      // panic("uvmunmap: walk");
    }

    if ((*pte & PTE_V) == 0) {
      continue;
    }
      // panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0) {
      // panic("uvmcopy: pte should exist");
      continue;
    }
      // panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0) {
      // non allocated memory
      continue;
    }
      // panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

#define VMASIZE 16
struct VMA {
  uint64 addr;
  uint64 len;
  uint64 original_addr;
  struct file *file;
  int permission;
  int flags;
  int pid;
};

struct VMA VMAs[VMASIZE];

uint64
sys_mmap(void)
{
  uint64 len, addr;
  int prot, flags;
  struct file *f;
  struct proc *p;

  // omit addr and offset
  if (argaddr(1, &len) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 || argfd(4, 0, &f) < 0) {
    return 0xffffffffffffffff;
  }

  struct VMA *vma = 0;
  for (int i = 0; i < VMASIZE; ++i) {
    if (!VMAs[i].file) {
      vma = &VMAs[i];
    }
  }

  if (!vma) {
    // fail
    return 0xffffffffffffffff;
  }

  if ((prot & PROT_WRITE) && !f->writable && !(flags & MAP_PRIVATE)) {
    return 0xffffffffffffffff;
  }

  p = myproc();
  addr = p->sz;
  p->sz += len; 
  vma->permission = prot;
  vma->flags = flags;
  vma->file = filedup(f);
  vma->pid = p->pid;
  vma->addr = addr;
  vma->len = len;
  vma->original_addr = addr;

  return addr;
}

int
unmmap(uint64 addr, uint len)
{
  struct VMA *vma = 0;
  struct proc *p = myproc();
  for (int i = 0; i < VMASIZE; ++i) {
    if (VMAs[i].file && VMAs[i].pid == p->pid) {
      vma = &VMAs[i];
      break;
    }
  }

  if (!vma || vma->addr > addr || vma->addr + vma->len < addr + len) {
    return -1;
  }

  if ((vma->flags & MAP_SHARED) && (vma->permission & PROT_WRITE)) {
    // write back
    if (filewrite_withoff(vma->file, addr, addr - vma->original_addr, len) < 0) {
      panic("filewrite_withoff");
    }
  }

  if (addr == vma->addr) {
    vma->addr = addr + len;
  }
  
  vma->len -= len;

  if (vma->len == 0) {
    fileclose(vma->file);
    vma->pid = -1;
    vma->file = 0;
    vma->addr = 0;
    vma->original_addr = 0;
    vma->len = 0;
    vma->permission = 0;
    vma->flags = 0;
  }

  uint64 start_addr = PGROUNDDOWN(addr);
  if (start_addr < addr) {
    start_addr += PGSIZE;
  }
  int page_to_unmap = 0;

  while (start_addr + PGSIZE <= addr + len) {
    ++page_to_unmap;
    start_addr += PGSIZE;
  }

  uvmunmap(p->pagetable, start_addr, page_to_unmap, 1);
  return 0;
}

uint64
sys_munmap(void)
{
  uint64 len, addr;
  if (argaddr(0, &addr) < 0 || argaddr(1, &len) < 0) {
    return -1;
  }

  return unmmap(addr, len);
}

int
mmap_trap(struct proc *p)
{
  int pid = p->pid;
  struct VMA *vma = 0;
  uint64 page_fault_addr;
  char *mem;

  page_fault_addr = r_stval();
  for (int i = 0; i < VMASIZE; ++i) {
    if (VMAs[i].file && VMAs[i].pid == pid) {
      vma = &VMAs[i];
      if (page_fault_addr < vma->addr || page_fault_addr >= vma->addr + vma->len) {
        vma = 0;
        continue;
      }

      break;
    }
  }

  if (!vma) {
    return -1;
  }

  mem = kalloc();
  if (!mem) {
    panic("kalloc");
    return -1;
  }

  memset(mem, 0, PGSIZE);
  uint64 start_addr = PGROUNDDOWN(page_fault_addr);
  int per = PTE_R | PTE_U;
  if (vma->permission & PROT_WRITE) {
    per |= PTE_W;
  }

  if (mappages(p->pagetable, start_addr, PGSIZE, (uint64)mem, per) != 0) {
    kfree(mem);
    panic("mmap fail for map lazy page");
  }

  uint off = page_fault_addr - vma->addr, n = PGSIZE - (page_fault_addr - start_addr);
  if (off + n > vma->len) {
    n = vma->len - off;
  }

  struct inode *ip = vma->file->ip;
  ilock(ip);
  int success = 0;
  if (readi(ip, 1, page_fault_addr, off, n) < 0) {
    panic("readi");
    success = -1;
  } 
  iunlock(ip);

  return success;
}

void
unmmap_by_pid(int pid)
{
  for (int i = 0; i < VMASIZE; ++i) {
    if (VMAs[i].file && VMAs[i].pid == pid) {
      unmmap(VMAs[i].addr, VMAs[i].len);
    }
  }
}           

int
mmap_fork(int oldpid, int newpid)
{
  int oldvma[VMASIZE] = {0}, ind = 0, emptyvma[VMASIZE] = {0}, emptyind = 0;
  for (int i = 0; i < VMASIZE; ++i) {
    if (VMAs[i].file && VMAs[i].pid == oldpid) {
      oldvma[ind++] = i;
      continue;
    }

    if (!VMAs[i].file) {
      emptyvma[emptyind++] = i;
    }
  }

  if (emptyind < ind) {
    printf("no enough empty VMA\n");
    return -1;
  }

  for (int i = 0; i < ind; ++i) {
    struct VMA *old = &VMAs[oldvma[i]];
    struct VMA *vma = &VMAs[emptyvma[i]];

    memmove(vma, old, sizeof(struct VMA));
    vma->pid = newpid;
    vma->file = filedup(old->file);
    printf("DUP from %d to %d\n", oldpid, newpid);
  }

  return 0;
}
