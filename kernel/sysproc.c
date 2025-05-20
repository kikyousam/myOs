#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
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
  backtrace();
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
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

uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler; // 临时变量存储用户传入的地址
  struct proc *p = myproc();

  // 获取用户参数并检查错误
  argint(0, &interval);
  argaddr(1, &handler);
  // printf("sys_sigalarm: interval=%d, handler=%p\n", interval, handler);

  // 赋值到 proc 结构体，并强制类型转换
  p->interval = interval;
  p->handle = (void (*)(void))handler; // 关键转换
  // printf("proc->interval=%d, proc->handler=%p\n", p->interval, p->handle);
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  if(p->trapframe_copy){
    *p->trapframe = *p->trapframe_copy;
    uint64 saved_a0 = p->trapframe_copy->a0;
    printf("Restoring a0: %p\n", p->trapframe->a0);
    kfree(p->trapframe_copy);
    p->trapframe_copy = 0;
  
    p->handler_active = 0;
    return saved_a0;
  }
  return -1;
}