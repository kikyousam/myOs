#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  uint64 start_va;
  int n_pages;
  uint64 bitmask_addr;
  argaddr(0, &start_va);
  argint(1, &n_pages);
  argaddr(2, &bitmask_addr);
  if(n_pages < 0 || n_pages > 64) return -1;
  struct proc* p =myproc();
  uint64 mask = 0;
  for(int i = 0; i < n_pages; ++i)
  {
    uint64 va = start_va + i*PGSIZE;
    pte_t* pte = walk(p->pagetable,va, 0);
    if(pte && (*pte & PTE_V) && (*pte & PTE_A))
    {
      mask |= 1UL << i;      //设置对应位
      *pte &= ~PTE_A;         //清除访问标志位
      sfence_vma();          //修改 PTE 后必须调用 `sfence_vma()` 使旧 TLB 失效，确保后续访问使用新 PTE.刷新TLB
    }
  }
  int bytes = (n_pages + 7) / 8;
  if(copyout(p->pagetable, bitmask_addr, (char *)&mask, bytes) < 0)
    return -1;
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
