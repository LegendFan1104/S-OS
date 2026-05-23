#include "types.h"
#include "platform.h"
#include "defs.h"
#include "param.h"
#include "mem/memlayout.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "fs/ext4/vfs_ext4_ext.h"
#include "sbi.h"
#include "proc/futex.h"

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

int
sys_wait4(void)
{
  int pid;
  uint64 status; 
  int options;
  
  argint(0, &pid);
  argaddr(1, &status);
  argint(2, &options);

  
  return wait4(pid, (int*)status, options);
}

int
sys_clone(void)
{
  int flags;
  uint64 stack, tls, ctid, ptid;
  argint(0, &flags);
  argaddr(1, &stack);
  argaddr(2, &ptid);
  argaddr(3, &tls);
  argaddr(4, &ctid);
  if (stack) {
    return clone(flags, (void*)stack, (int*)(uint64)ptid, tls, (int*)(uint64)ctid);
  } else {
    return fork();
  }

}


uint64 sys_shutdown(void) {
#ifdef RISCV
  struct filesystem *fs = get_fs_from_path("/");
  vfs_ext_umount(fs);
  sbi_shutdown();
  for(;;) {}
#endif
#ifdef LOONGARCH
  *(volatile uint8 *)(0x8000000000000000 | 0x100E001C) = 0x34;
  for(;;) {}
#endif
}

uint64
sys_getppid(void)
{ 
  return getppid();
}

//busybox need
int
sys_set_tid_address(void)
{
    uint64 tidptr;
    
    argaddr(0, &tidptr);
    struct proc *p = myproc();
    p->clear_child_tid = tidptr;
    return p->pid;
}

int
sys_getuid(void)
{
  return 0;
}
int
sys_getgid(void)
{
  return 0;
}

int
sys_setuid(void)
{
  int uid;
  argint(0, &uid);

  if(myproc()->uid != ROOT_UID && uid != myproc()->uid)
    return -1;
  
  myproc()->uid = uid;
  return 0;
}

int
sys_setgid(void)
{
  int gid;
  argint(0, &gid);
  if(myproc()->uid != ROOT_UID && gid != myproc()->gid)
    return -1;
  
  myproc()->gid = gid;
  return 0;
}

uint64 sys_exit_group(void) {
  int status;
  argint(0, &status);
  exit(status);
  return 0;  // not reached
}

uint64 sys_gettid(void) {
  return myproc()->pid;
}

//在线程崩溃后将list中的锁释放，唤醒等待线程
uint64 sys_set_robust_list(void) {
  return 0;
}

//限制进程资源
uint64 sys_prlimit64(void) {
  return 0;
}

uint64 sys_getpgid(void) {
  return 0;
}

uint64 sys_geteuid(void) {
  return 0;
}

uint64 sys_setpgid(void) {
  return 0;
}


uint64
sys_sysinfo(void)
{
  struct sysinfo info;
  struct proc *p = myproc();
  uint64 addr;
  argaddr(0, &addr);
  info.uptime = ticks;
  info.procs = countprocs();
  info.freeram = 0;
  if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}

uint64
sys_clock_nanosleep(void)
{
    uint64 req_addr, rem_addr;
    argaddr(1, &req_addr);
    argaddr(2, &rem_addr);
    struct timespec req, rem;
    if (copyin(myproc()->pagetable, (char*)(&req), req_addr, sizeof(req)) < 0) {
        return -1;
    }
    if (copyin(myproc()->pagetable, (char*)(&rem), rem_addr, sizeof(rem)) < 0) {
        return -1;
    }
    uint64 n = req.tv_sec;
    acquire(&tickslock);
    uint ticks0;  
    ticks0 = ticks;
    while(ticks - ticks0 < n){
        if(killed(myproc())){
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    struct timespec remp = {0, 0};
    copyout(myproc()->pagetable, (uint64)rem_addr, (char*)&remp, sizeof(remp));
    release(&tickslock);
    return 0;
}

uint64 sys_getegid(void) {
  return 0;
}

uint64 sys_futex(void) {
  uint64 uaddr, timeoutaddr, uaddr2;
  int futex_op, val, val3;

  argaddr(0, &uaddr);
  argint(1, &futex_op);
  argint(2, &val);
  argaddr(3, &timeoutaddr);
  argaddr(4, &uaddr2);
  argint(5, &val3);

  struct proc *p = myproc();

  futex_op &= ~FUTEX_PRIVATE_FLAG;
  switch (futex_op)
  {
    case FUTEX_WAIT:
      acquire(&p->lock);
    int futex_word;
    copyin(p->pagetable, (char*)&futex_word, uaddr, sizeof(int));
    // printf("[sys_futex] futex_word: %d\n", futex_word);
    if(futex_word != val){
      release(&p->lock);
      return -11;
    }

    struct timespec ts = { 0 };
    uint64 n, timestamp;
    if(timeoutaddr){
      if(copyin(p->pagetable, (char*)&ts, timeoutaddr, sizeof(struct timespec)) < 0)
        return -1;
      n = (ts.tv_sec + 3) * FREQUENCY + (ts.tv_nsec * FREQUENCY) / 1000000000;

      timestamp = rdtime();
      while(rdtime() - timestamp < n){
        if(p->killed == SIGKILL){
          release(&p->lock);
          return -1;
        }
        sleep1(&ticks, (void*)uaddr, &p->lock);
        if(p->chan2 == 0){
          // printf("[sys_futex] pid %d woke up before timeout!\n", p->pid);
          release(&p->lock);
          return 0;
        }
      }
      p->chan2 = 0;
      release(&p->lock);
      // printf("[sys_futex] pid %d woke up from timeout!\n", p->pid);
      return 0;
    }

    if(val == -1){
      futex_word = 1;
      copyout(p->pagetable, uaddr, (char*)&futex_word, sizeof(int));
      release(&p->lock);
      return 0;
    }

    sleep1((void*)uaddr, (void*)uaddr, &p->lock);
    release(&p->lock);
    // printf("[sys_futex] pid %d woke up!\n", p->pid);
    return 0;
    case FUTEX_WAKE:
      return wakeup2((void*)uaddr, val, NULL, 0);
    case FUTEX_REQUEUE:
      return wakeup2((void*)uaddr, val, (void*)uaddr2, (int)timeoutaddr);
    default:
      panic("unknown futex operand!\n");
  }

  return 0;

}
















