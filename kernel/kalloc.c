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
  struct spinlock lock[NCPU];
  struct run *freelist[NCPU];
} kmemPerCpu;

void
kinit()
{
  push_off();
  for(int i = 0; i < NCPU; ++i) {
    initlock(&kmemPerCpu.lock[i], "kmemPerCpu");
  }
  freerange(end, (void*)PHYSTOP);
  pop_off();
}

void freerange(void *pa_start, void *pa_end) {
  char *p = (char*)PGROUNDUP((uint64)pa_start);
  int cpuId = 0;  // 让所有 CPU 轮流分配内存块

  for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    struct run *r = (struct run*)p;
    push_off();
    acquire(&kmemPerCpu.lock[cpuId]);
    r->next = kmemPerCpu.freelist[cpuId];
    kmemPerCpu.freelist[cpuId] = r;
    release(&kmemPerCpu.lock[cpuId]);
    pop_off();

    // 轮流切换到下一个 CPU，避免所有块分配给同一个 CPU
    cpuId = (cpuId + 1) % NCPU;
  }
}


// Free the page of physical memory pointed at by pa,
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
  int cpuId = cpuid();
  acquire(&kmemPerCpu.lock[cpuId]);
  r->next = kmemPerCpu.freelist[cpuId];
  kmemPerCpu.freelist[cpuId] = r;
  release(&kmemPerCpu.lock[cpuId]);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpuId = cpuid();
  acquire(&kmemPerCpu.lock[cpuId]);
  r = kmemPerCpu.freelist[cpuId];
  if(r)
    kmemPerCpu.freelist[cpuId] = r->next;
  else{
    for(int i = 0; i < NCPU; ++i){
    if(i == cpuId) continue;
    acquire(&kmemPerCpu.lock[i]);
    r = kmemPerCpu.freelist[i];
    if(r) { // 找到空闲块后立刻退出循环
      kmemPerCpu.freelist[i] = r->next; // 正确更新该 CPU 的 free list
      release(&kmemPerCpu.lock[i]);
      break;  // 立即停止循环
    }
    release(&kmemPerCpu.lock[i]);
  }

  }
  release(&kmemPerCpu.lock[cpuId]);
  pop_off();
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
