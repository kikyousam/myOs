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
int ref_count[(PHYSTOP - KERNBASE) / PGSIZE] = {0};
struct spinlock ref_cnt_lock;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref_cnt_lock, "refcnt");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int ref;
  
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  
  // 原子操作：减少引用计数并检查
  acquire(&ref_cnt_lock);
  ref_count[PA2IDX(pa)]--;
  ref = ref_count[PA2IDX(pa)];
  release(&ref_cnt_lock);

  if (ref > 0) 
    return; // 仍有其他引用，不释放物理页
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void* kalloc(void) {
  struct run *r;

  // 先获取 kmem.lock 操作空闲链表
  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
  }
  release(&kmem.lock); // 尽快释放 kmem.lock

  if (r) {
    // 再处理引用计数（与其他代码统一锁顺序）
    acquire(&ref_cnt_lock);
    ref_count[PA2IDX(r)] = 1; // 明确初始化为1
    release(&ref_cnt_lock);

    memset((char*)r, 5, PGSIZE);
  }

  return (void*)r;
}