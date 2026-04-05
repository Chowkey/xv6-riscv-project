#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S
// -----------------------------------------------------------------------
// Shared Memory (mmap/munmap) — multi-region keyed implementation
// -----------------------------------------------------------------------
//
// Design:
//   Global table:  shmem_table[MAX_SHMEM]  — one entry per shared region.
//     Each entry stores the physical address, reference count, key, and
//     its own spinlock so independent regions never contend on each other.
//
//   Per-process:  proc->shmem_mappings[MAX_PROC_SHMEM]
//     Tracks (key, va) pairs for every region this process has mapped.
//     proc->shmem_next_va is a bump pointer for dynamic VA assignment;
//     it starts at SHMEM_BASE and advances one page per mapping.
//
// Virtual address layout (user space):
//   [0 .. p->sz)        normal process heap/stack
//   [SHMEM_BASE ..)     shared memory window — grows upward
//
// Key semantics:
//   Two processes that call mmap() with the same key share the same
//   physical page.  A key of 0 is invalid; keys 1..INT_MAX are valid.
// -----------------------------------------------------------------------

#define SHMEM_BASE 0x4000000UL  // 64 MB — start of shared-memory window

// One entry in the global shared-region table
struct shmem_entry {
  int    key;        // >0 means allocated; 0 means free
  uint64 pa;         // physical address of the shared page
  int    refcount;   // number of processes that have mapped this region
  struct spinlock lock;
};

static struct shmem_entry shmem_table[MAX_SHMEM];
static struct spinlock     shmem_table_lock;  // protects allocation/search

// Called once at boot (before kvminit)
void
init_shmem(void)
{
  initlock(&shmem_table_lock, "shmem_tbl");
  for(int i = 0; i < MAX_SHMEM; i++){
    initlock(&shmem_table[i].lock, "shmem_entry");
    shmem_table[i].key      = 0;
    shmem_table[i].pa       = 0;
    shmem_table[i].refcount = 0;
  }
}

// Called from allocproc() to initialize a new process's shmem state
void
proc_shmem_init(struct proc *p)
{
  for(int i = 0; i < MAX_PROC_SHMEM; i++){
    p->shmem_mappings[i].key = -1;
    p->shmem_mappings[i].va  = 0;
  }
  p->shmem_next_va = SHMEM_BASE;
}

// Find the global table entry for a key (caller must hold shmem_table_lock)
static struct shmem_entry *
shmem_find_entry(int key)
{
  for(int i = 0; i < MAX_SHMEM; i++)
    if(shmem_table[i].key == key)
      return &shmem_table[i];
  return 0;
}

// Map the shared region identified by `key` into the calling process.
// Returns the virtual address the region is mapped at, or 0 on failure.
uint64
mmap(int key)
{
  struct proc *p;
  struct shmem_entry *entry;
  uint64 va, pa;
  int slot;

  if(key <= 0)
    return 0;

  p = myproc();

  // --- Check if this process already mapped this key ---
  for(int i = 0; i < MAX_PROC_SHMEM; i++){
    if(p->shmem_mappings[i].key == key)
      return p->shmem_mappings[i].va;  // idempotent: return existing mapping
  }

  // --- Find a free slot in the process mapping table ---
  slot = -1;
  for(int i = 0; i < MAX_PROC_SHMEM; i++){
    if(p->shmem_mappings[i].key == -1){
      slot = i;
      break;
    }
  }
  if(slot == -1)
    return 0;  // process has too many shmem mappings

  // --- Look up or create the global entry for this key ---
  acquire(&shmem_table_lock);

  entry = shmem_find_entry(key);
  if(entry == 0){
    // First process to request this key — allocate a new entry
    for(int i = 0; i < MAX_SHMEM; i++){
      if(shmem_table[i].key == 0){
        entry = &shmem_table[i];
        break;
      }
    }
    if(entry == 0){
      release(&shmem_table_lock);
      return 0;  // global table full
    }
    // Allocate physical page
    char *mem = kalloc();
    if(mem == 0){
      release(&shmem_table_lock);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    entry->key      = key;
    entry->pa       = (uint64)mem;
    entry->refcount = 1;
  } else {
    // Existing region — bump refcount
    acquire(&entry->lock);
    entry->refcount++;
    release(&entry->lock);
  }
  pa = entry->pa;
  release(&shmem_table_lock);

  // --- Assign a virtual address from this process's shmem window ---
  va = p->shmem_next_va;
  p->shmem_next_va += PGSIZE;

  // --- Map pa into the process page table ---
  if(mappages(p->pagetable, va, PGSIZE, pa, PTE_R | PTE_W | PTE_U) != 0){
    // Roll back refcount
    acquire(&shmem_table_lock);
    acquire(&entry->lock);
    entry->refcount--;
    if(entry->refcount == 0){
      kfree((void *)entry->pa);
      entry->pa  = 0;
      entry->key = 0;
    }
    release(&entry->lock);
    release(&shmem_table_lock);
    p->shmem_next_va -= PGSIZE;
    return 0;
  }

  // --- Record in per-process table ---
  p->shmem_mappings[slot].key = key;
  p->shmem_mappings[slot].va  = va;

  return va;
}

// Unmap the shared region that is mapped at `addr` in the calling process.
// Returns 0 on success, -1 on error.
int
munmap(uint64 addr)
{
  struct proc *p;
  struct shmem_entry *entry;
  pte_t *pte;
  uint64 pa;
  int slot;
  int key;

  p = myproc();

  // Find which key is at this address in the process's mapping table
  slot = -1;
  key  = -1;
  for(int i = 0; i < MAX_PROC_SHMEM; i++){
    if(p->shmem_mappings[i].key != -1 && p->shmem_mappings[i].va == addr){
      slot = i;
      key  = p->shmem_mappings[i].key;
      break;
    }
  }
  if(slot == -1)
    return -1;  // not a shmem address owned by this process

  // Verify the PTE is valid
  pte = walk(p->pagetable, addr, 0);
  if(pte == 0 || (*pte & PTE_V) == 0)
    return -1;

  pa = PTE2PA(*pte);

  // Clear the PTE
  *pte = 0;

  // Drop the reference in the global table
  acquire(&shmem_table_lock);
  entry = shmem_find_entry(key);
  if(entry != 0){
    acquire(&entry->lock);
    entry->refcount--;
    if(entry->refcount == 0){
      kfree((void *)entry->pa);
      entry->pa  = 0;
      entry->key = 0;
    }
    release(&entry->lock);
  } else {
    // Orphaned page — free it directly
    kfree((void *)pa);
  }
  release(&shmem_table_lock);

  // Clear the per-process slot
  p->shmem_mappings[slot].key = -1;
  p->shmem_mappings[slot].va  = 0;

  return 0;
}

// Unmap ALL shared regions for a process — called on exit/exec.
void
shmem_unmap_all(struct proc *p)
{
  for(int i = 0; i < MAX_PROC_SHMEM; i++){
    if(p->shmem_mappings[i].key != -1)
      munmap(p->shmem_mappings[i].va);
  }
}



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
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
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

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
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
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
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

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
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
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
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
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
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
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
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
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
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

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}
