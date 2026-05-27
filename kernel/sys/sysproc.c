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
#include "lib/string.h"

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
    uint64 req_addr;
    int clockid, flags;
    argint(0, &clockid);
    argint(1, &flags);
    argaddr(2, &req_addr);
    // rem_addr not used — skip argaddr to avoid pagefault_handler side effect
    struct timespec req;
    if (copyin(myproc()->pagetable, (char*)(&req), req_addr, sizeof(req)) < 0) {
        return -1;
    }
    // If TIMER_ABSTIME, convert to relative time first
    if (flags == TIMER_ABSTIME) {
        uint64 now = rdtime();
        uint64 req_ns = req.tv_sec * 1000000000ULL + req.tv_nsec;
        uint64 now_ns = now / FREQUENCY * 1000000000ULL + (now % FREQUENCY) * 1000000000ULL / FREQUENCY;
        if (req_ns <= now_ns) {
            return 0;
        }
        uint64 diff_ns = req_ns - now_ns;
        req.tv_sec = diff_ns / 1000000000ULL;
        req.tv_nsec = diff_ns % 1000000000ULL;
    } else {
        // Cap relative-time sleeps to avoid hangs from buggy userspace
        if (req.tv_sec > 60)
            req.tv_sec = 60;
    }
    // Convert relative time to ticks
    uint64 n64 = req.tv_sec * FREQUENCY / INTERVAL;
    int n = (n64 > 0x7FFFFFFFULL) ? 0x7FFFFFFF : (int)n64;
    if (n <= 0) n = 1;
    acquire(&tickslock);
    uint ticks0 = ticks;
    while((int)(ticks - ticks0) < n){
        if(killed(myproc())){
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

uint64 sys_getegid(void) {
  return 0;
}

// prctl - process control
uint64 sys_prctl(void) {
  int option;
  uint64 arg2;
  argint(0, &option);
  argaddr(1, &arg2);

  switch (option) {
  case 1:  // PR_SET_PDEATHSIG
    return 0;
  case 2:  // PR_GET_PDEATHSIG
    return 0;
  case 3:  // PR_GET_DUMPABLE
    return 1;
  case 4:  // PR_SET_DUMPABLE
    return 0;
  case 5:  // PR_GET_NAME (deprecated, but used)
    return 0;
  case 9:  // PR_SET_MM
    return 0;
  case 11: // PR_CAPBSET_READ
    return 1;
  case 12: // PR_CAPBSET_DROP
    return 0;
  case 15: // PR_SET_NAME
    return 0;
  case 16: // PR_GET_NAME
    if (arg2) {
      copyout(myproc()->pagetable, arg2, myproc()->name, 16);
    }
    return 0;
  case 22: // PR_SET_SECCOMP
    return 0;
  case 23: // PR_GET_SECCOMP
    return 0;
  case 32: // PR_SET_THP_DISABLE
    return 0;
  case 33: // PR_GET_THP_DISABLE
    return 0;
  case 35: // PR_GET_TID_ADDRESS
    return 0;
  case 43: // PR_SET_VMA
    return 0;
  case 47: // PR_GET_SPECULATION_CTRL
    return 0;
  case 53: // PR_SET_TIMERSLACK
    return 0;
  case 54: // PR_GET_TIMERSLACK
    return 0;
  default:
    return 0;
  }
}

uint64 sys_getrlimit(void) {
  int resource;
  uint64 rlim_addr;
  argint(0, &resource);
  argaddr(1, &rlim_addr);
  // Return a generous default for all resources
  struct { uint64 cur; uint64 max; } rlim;
  rlim.cur = 0xFFFFFFFFFFFFFFFFUL;
  rlim.max = 0xFFFFFFFFFFFFFFFFUL;
  if (copyout(myproc()->pagetable, rlim_addr, (char*)&rlim, sizeof(rlim)) < 0)
    return -1;
  return 0;
}

uint64 sys_setrlimit(void) {
  return 0;  // accept any limit
}

uint64 sys_getrusage(void) {
  int who;
  uint64 rusage_addr;
  argint(0, &who);
  argaddr(1, &rusage_addr);
  // struct rusage is 144 bytes on 64-bit Linux
  char buf[144];
  memset(buf, 0, sizeof(buf));
  // Fill in ru_utime: tv_sec = ticks / frequency, tv_usec
  struct proc *p = myproc();
  uint64 *utime_sec = (uint64*)(buf + 0);
  uint64 *utime_usec = (uint64*)(buf + 8);
  uint64 *stime_sec = (uint64*)(buf + 16);
  uint64 *stime_usec = (uint64*)(buf + 24);
  *utime_sec = p->proc_tms.tms_utime / FREQUENCY;
  *utime_usec = (p->proc_tms.tms_utime % FREQUENCY) * 1000000 / FREQUENCY;
  *stime_sec = p->proc_tms.tms_stime / FREQUENCY;
  *stime_usec = (p->proc_tms.tms_stime % FREQUENCY) * 1000000 / FREQUENCY;
  if (copyout(p->pagetable, rusage_addr, buf, sizeof(buf)) < 0)
    return -1;
  return 0;
}

uint64 sys_getpriority(void) {
  int which, who;
  argint(0, &which);
  argint(1, &who);
  // Return nice value (0-39, where 20 is neutral)
  return 20 - myproc()->sched_priority;
}

uint64 sys_setpriority(void) {
  int which, who, prio;
  argint(0, &which);
  argint(1, &who);
  argint(2, &prio);
  return 0;
}

uint64 sys_umask(void) {
  int mask;
  argint(0, &mask);
  struct proc *p = myproc();
  int old = p->umask;
  p->umask = mask;
  return old;
}

uint64 sys_personality(void) {
  uint64 persona;
  argaddr(0, &persona);
  static uint64 current_persona = 0;
  uint64 old = current_persona;
  if (persona != 0xFFFFFFFFFFFFFFFFUL)
    current_persona = persona;
  return old;
}

uint64 sys_setresuid(void) {
  int ruid, euid, suid;
  argint(0, &ruid);
  argint(1, &euid);
  argint(2, &suid);
  return 0;
}

uint64 sys_getresuid(void) {
  uint64 ruid_addr, euid_addr, suid_addr;
  argaddr(0, &ruid_addr);
  argaddr(1, &euid_addr);
  argaddr(2, &suid_addr);
  struct proc *p = myproc();
  if (ruid_addr && copyout(p->pagetable, ruid_addr, (char*)&p->uid, sizeof(int)) < 0)
    return -1;
  if (euid_addr && copyout(p->pagetable, euid_addr, (char*)&p->uid, sizeof(int)) < 0)
    return -1;
  if (suid_addr && copyout(p->pagetable, suid_addr, (char*)&p->uid, sizeof(int)) < 0)
    return -1;
  return 0;
}

uint64 sys_setresgid(void) {
  int rgid, egid, sgid;
  argint(0, &rgid);
  argint(1, &egid);
  argint(2, &sgid);
  return 0;
}

uint64 sys_getresgid(void) {
  uint64 rgid_addr, egid_addr, sgid_addr;
  argaddr(0, &rgid_addr);
  argaddr(1, &egid_addr);
  argaddr(2, &sgid_addr);
  struct proc *p = myproc();
  if (rgid_addr && copyout(p->pagetable, rgid_addr, (char*)&p->gid, sizeof(int)) < 0)
    return -1;
  if (egid_addr && copyout(p->pagetable, egid_addr, (char*)&p->gid, sizeof(int)) < 0)
    return -1;
  if (sgid_addr && copyout(p->pagetable, sgid_addr, (char*)&p->gid, sizeof(int)) < 0)
    return -1;
  return 0;
}

uint64 sys_getgroups(void) {
  int gidsetsize;
  uint64 grouplist_addr;
  argint(0, &gidsetsize);
  argaddr(1, &grouplist_addr);
  if (gidsetsize > 0 && grouplist_addr) {
    int gid = myproc()->gid;
    if (copyout(myproc()->pagetable, grouplist_addr, (char*)&gid, sizeof(int)) < 0)
      return -1;
    return 1;
  }
  return 1;
}

uint64 sys_setgroups(void) {
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
















