#include "types.h"
#include "param.h"
#include "mem/memlayout.h"
#include "platform.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "proc/futex.h"

#include "fs/vfs/fs.h"

#include "defs.h"
#include "lib/string.h"
#include "mem/mem.h"


struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

int interrupts_count[1024];

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S
extern char sig_trampoline[]; // sig_trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
#ifdef RISCV
  for(p = proc; p < &proc[NPROC]; p++) {
    uint64 va = KSTACK((int) (p - proc));
    for (uint64 off = 0; off < KSTACKSIZE; off += PGSIZE) {
      char *pa = kalloc();
      if(pa == 0)
        panic("kalloc");
      kvmmap(kpgtbl, va + off, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
  }
#elif defined(LOONGARCH)

  for(p = proc; p < &proc[NPROC]; p++) {
    uint64 va = KSTACK((int) (p - proc));
    for (uint64 off = 0; off < KSTACKSIZE; off += PGSIZE) {
      char *pa = kalloc();
      if(pa == 0)
        panic("kalloc");
      if(mappages(kpgtbl, va + off, PGSIZE, (uint64)pa,  PTE_NX | PTE_P | PTE_W | PTE_MAT | PTE_D | PTE_PLV) != 0)
        panic("kvmmap");
    }

  }
#endif
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);
  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->uid = 0;
  p->gid = 0;
  p->pid = allocpid();
  p->state = USED;

  // 初始化调度相关字段
  p->sched_policy = SCHED_OTHER;
  p->sched_priority = SCHED_DEFAULT_PRIORITY;
  p->cpu_affinity = 0xFFFFFFFF;  // 默认允许在所有CPU上运行

  // printf("%p\n", p);

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  //Signal
  p->sig = (struct sighand*)kmalloc(sizeof(struct sighand));
  p->block.val = 0;


  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + KSTACKSIZE;
  memset(&p->vma, 0, NVMA * sizeof(struct vm_area));
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{

  if(p->trapframe)
    kfree((void*)p->trapframe);

  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);

  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;


  //Signal
  p->sig_frame = 0;
  p->block.val = 0;

  if (p->sig) {
    kfree(p->sig);
  }
  p->sig = 0;

  memset((void*)((p->kstack)), 0, KSTACKSIZE);

  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;
#ifdef RISCV
  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  if(mappages(pagetable, SIG_TRAMPOLINE, PGSIZE,
            (uint64)sig_trampoline, PTE_R | PTE_X | PTE_U) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
            }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, SIG_TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }
#elif defined(LOONGARCH)
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_NX | PTE_P | PTE_W | PTE_MAT | PTE_D) < 0){
    uvmfree(pagetable, 0);
    return 0;
              }


  if(mappages(pagetable, SIG_TRAMPOLINE, PGSIZE,
          (uint64)sig_trampoline, PTE_P | PTE_MAT | PTE_D) < 0) {
    printf("Fail to map sig_trampoline\n");
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
          }


#endif
  static int first = 0;
  if (!first) {
    printf("sig_trampoline: %p %p\n",sig_trampoline, walk(pagetable, SIG_TRAMPOLINE, 0));
    first = 1;
  }
  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
#ifdef RISCV
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
#endif
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmunmap(pagetable, SIG_TRAMPOLINE, 1, 0);


  uvmfree(pagetable, sz);
}

#ifdef RISCV
extern uchar initcode_start[];
extern uchar initcode_end[];

#elif defined(LOONGARCH)
extern uchar initcode_start[];
extern uchar initcode_end[];

#endif
// Set up first user process.
void
userinit(void)
{
#ifdef RISCV
  struct proc *p;
  uint64 init_sz;
  uint64 user_sz;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  init_sz = initcode_end - initcode_start;
  uvmfirst(p->pagetable, initcode_start, init_sz);
  user_sz = PGROUNDUP(init_sz) + 2 * PGSIZE;
  p->sz = user_sz;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0; //user program counter
  p->trapframe->sp = user_sz;  // user stack pointer


  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd.fs = get_fs_by_type(EXT4);
  strcpy(p->cwd.path, "/");

  p->state = RUNNABLE;

  release(&p->lock);
#elif defined(LOONGARCH)
  struct proc *p;
  uint64 init_sz;
  uint64 user_sz;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  init_sz = initcode_end - initcode_start;
  uvmfirst(p->pagetable, initcode_start, init_sz);
  user_sz = PGROUNDUP(init_sz) + 2 * PGSIZE;
  p->sz = user_sz;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->era = 0; //user program counter
  p->trapframe->sp = user_sz;  // user stack pointer


  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd.fs = get_fs_by_type(EXT4);
  strcpy(p->cwd.path, "/");

  p->state = RUNNABLE;

  release(&p->lock);
#endif
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int64 n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if (sz >= MAXVA - PGSIZE)
      return -1;
    if ((uint64)n > (MAXVA - PGSIZE - sz))
      return -1;
#ifdef RISCV
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
#elif defined(LOONGARCH)
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_P|PTE_W|PTE_PLV|PTE_MAT|PTE_D)) == 0) {
      return -1;
    }
#endif
  } else if(n < 0){
    uint64 dec = (uint64)(-n);
    if (dec > sz)
      return -1;
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  acquire(&p->lock);
  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);
  np->parent = p;  // 设置父进程
  release(&p->lock);
  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = get_fops()->dup(p->ofile[i]);

  for(i = 0; i < NVMA; ++i) {
    if(p->vma[i].used) {
      memmove(&np->vma[i], &p->vma[i], sizeof(p->vma[i]));
      if(p->vma[i].vfile)
        get_fops()->dup(p->vma[i].vfile);
    }
  }

  np->cwd.fs = p->cwd.fs;
  strcpy(np->cwd.path, p->cwd.path);

  strcpy(np->name, p->name);

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);


  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Clean up robust futex list before closing files
  // This wakes up any threads waiting on futexes we own
  futex_robust_list_cleanup(p);
  futex_clear_waiter(p);

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      get_fops()->close(f);
      p->ofile[fd] = 0;
    }
  }

  // 将进程的已映射区域取消映射
  for(int i = 0; i < NVMA; ++i) {
    if(p->vma[i].used) {
      if (p->vma[i].vfile != NULL) {
        if(p->vma[i].flags == MAP_SHARED && (p->vma[i].prot & PROT_WRITE) != 0) {
          get_fops()->write(p->vma[i].vfile, p->vma[i].addr, p->vma[i].len);
        }
        get_fops()->close(p->vma[i].vfile);
      }
      uvmunmap(p->pagetable, p->vma[i].addr, p->vma[i].len / PGSIZE, 1);
      p->vma[i].used = 0;
    }
  }
  // printf("Here\n");
  memset(&(p->cwd), 0, sizeof(p->cwd));

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  //SYS_set_tid_address
  if (p->clear_child_tid) {
    int clear = 0;
    // Ignore copyout error - address might be invalid if user corrupted it
    copyout(p->pagetable, p->clear_child_tid, (char*)&clear, sizeof(clear));
    p->clear_child_tid = 0;
  }

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          // printf("pid:%d xstate:%d\n", pp->pid, pp->xstate);
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
            sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  struct proc *highest_p;
  int highest_priority;
  int current_cpu = cpuid();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    highest_p = 0;
    highest_priority = -1;

    // First pass: find the highest priority runnable process
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Check CPU affinity
        if ((p->cpu_affinity & (1 << current_cpu)) == 0) {
          release(&p->lock);
          continue;
        }
        
        // Calculate effective priority
        // Real-time policies (FIFO, RR) have higher priority than normal
        int effective_priority = p->sched_priority;
        if (p->sched_policy == SCHED_FIFO || p->sched_policy == SCHED_RR) {
          effective_priority += 100;  // Boost RT processes
        }
        
        if (effective_priority > highest_priority) {
          if (highest_p) {
            release(&highest_p->lock);
          }
          highest_p = p;
          highest_priority = effective_priority;
        } else {
          release(&p->lock);
        }
      } else {
        release(&p->lock);
      }
    }

    // Run the highest priority process if found
    if (highest_p) {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      highest_p->state = RUNNING;
      c->proc = highest_p;
      swtch(&c->context, &highest_p->context);

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      release(&highest_p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();
  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    // printf("sp: %x\n", r_sp());
    filesystem_init();
    filesystem2_init();//启动init
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

int kill_signal(int pid, int sig) {
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->signal = sig;
    }
    release(&p->lock);
  }
  return 0;
}

int tkill(int pid, int sig) {
  struct proc *p;
  // printf("222\n");
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->signal = sig;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  // printf("111\n");
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}


int
wait4(int pid, int *status, int options)
{
  struct proc *pp;
  int havekids, found_pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    havekids = 0;
    found_pid = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        acquire(&pp->lock);

        havekids = 1;
        
        if(pid != -1 && pp->pid != pid){
          release(&pp->lock);
          continue;
        }
        if((options & WNOHANG) && pp->state != ZOMBIE){
          found_pid = 0;
          release(&pp->lock);
          continue;
        }

        if(pp->state == ZOMBIE){
          found_pid = pp->pid;
          pp->xstate = pp->xstate<<8;
          // printf("pid: %d, exit: %d\n", pp->pid, pp->xstate);
          if(status != 0 && copyout(p->pagetable, (uint64)status, 
            (char *)&pp->xstate, sizeof(pp->xstate)) < 0) 
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return found_pid;
        }
        release(&pp->lock);
      }
    }
    if(!havekids || killed(p)){
      release(&wait_lock);
      return 0;
    }
    
    sleep(p, &wait_lock);
  }
}

// waitid implementation
int waitid(int idtype, int id, siginfo_t *infop, int options)
{
  struct proc *pp;
  int havekids, found_pid;
  struct proc *p = myproc();
  siginfo_t info;

  acquire(&wait_lock);

  for(;;){
    havekids = 0;
    found_pid = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        acquire(&pp->lock);

        havekids = 1;
        
        // Check idtype and id
        if(idtype == P_PID && pp->pid != id){
          release(&pp->lock);
          continue;
        }
        if(idtype == P_PGID && pp->pgid != id){
          release(&pp->lock);
          continue;
        }
        
        if((options & WNOHANG) && pp->state != ZOMBIE){
          found_pid = 0;
          release(&pp->lock);
          continue;
        }

        if(pp->state == ZOMBIE){
          found_pid = pp->pid;
          
          // Fill in siginfo
          memset(&info, 0, sizeof(info));
          info.si_signo = SIGCHLD;
          info.si_code = CLD_EXITED;
          info.si_pid = pp->pid;
          info.si_uid = pp->uid;
          info.si_status = pp->xstate;
          
          if(infop != 0 && copyout(p->pagetable, (uint64)infop, 
            (char *)&info, sizeof(info)) < 0) 
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          
          if(!(options & WNOWAIT)) {
            freeproc(pp);
          }
          release(&pp->lock);
          release(&wait_lock);
          return 0;  // waitid returns 0 on success
        }
        release(&pp->lock);
      }
    }
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -10;  // -ECHILD
    }
    
    sleep(p, &wait_lock);
  }
}


int
clone(unsigned long flags, void *stack, int *ptid, unsigned long tls, int *ctid)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // Copy saved user registers.
  *(np->trapframe) = *(p->trapframe);
  np->parent = p;  // 设置父进程
  // Cause clone to return 0 in the child.
  np->trapframe->a0 = 0;

  // Set new stack pointer if specified
  if(stack != 0) {
    np->trapframe->sp = (uint64)stack;
  }

  // Handle file descriptors based on flags
  if(!(flags & CLONE_FILES)) {
    // Default behavior - duplicate file descriptors
    for(i = 0; i < NOFILE; i++) {
      if(p->ofile[i]) {
        np->ofile[i] = get_fops()->dup(p->ofile[i]);
      }
    }
  } else {
    // Share file descriptor table - just copy pointers
    for(i = 0; i < NOFILE; i++) {
      np->ofile[i] = p->ofile[i];
      if(p->ofile[i]) {
        // Increment reference count using dup instead of hold
        get_fops()->dup(p->ofile[i]);
      }
    }
  }

  for(i = 0; i < NVMA; ++i) {
    if(p->vma[i].used) {
      if(!(flags & CLONE_VM)) {
        memmove(&np->vma[i], &p->vma[i], sizeof(p->vma[i]));
        if(p->vma[i].vfile)
          get_fops()->dup(p->vma[i].vfile);
      } else {
        np->vma[i] = p->vma[i];
        if(p->vma[i].vfile)
          get_fops()->dup(p->vma[i].vfile);
      }
    }
  }

  if(!(flags & CLONE_FS)) {
    np->cwd.fs = p->cwd.fs;
    strcpy(np->cwd.path, p->cwd.path);
  } else {
    np->cwd = p->cwd;
  }

  strcpy(np->name, p->name);

  if(ptid != 0) {
    if(copyout(np->pagetable, (uint64)ptid, (char *)&p->pid, sizeof(p->pid)) < 0) {
      freeproc(np);
      release(&np->lock);
      return -1;
    }
  }

  if(ctid != 0) {
    if(copyout(np->pagetable, (uint64)ctid, (char *)&np->pid, sizeof(np->pid)) < 0) {
      freeproc(np);
      release(&np->lock);
      return -1;
    }
  }

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  if(flags & CLONE_PARENT) {
    np->parent = p->parent;
  } else {
    np->parent = p;
  }
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

uint64
getppid(void)
{
  struct proc *curproc = myproc();
  
  if(curproc->parent == 0) {
    return 1;
  }
  
  return curproc->parent->pid;
}
// 计算当前运行的进程数量
uint64
countprocs(void)
{
  struct proc *p;
  uint64 count = 0;
  
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED) {
      count++;
    }
    release(&p->lock);
  }
  
  return count;
}

int read_interrupts(char *context) {
  char *be = context;
  for (int i=0; i<1023;i++) {
    if (interrupts_count[i]== 0) {
      continue;
    }
    context = digit_tostring(context, i);
    *context = ':';
    context++;
    *context = ' ';
    context++;
    context = digit_tostring(context, interrupts_count[i]);
    *context = '\n';
    context++;
  }
  context--;
  *context = '\0';
  printf(" \b");
  return (int)((uint64)context - (uint64)be);
}

void receive_interrupts(int irq) {
  interrupts_count[irq]++;
}

void
sleep1(void *chan, void *chan2, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(lk != &p->lock){
    acquire(&p->lock);
    release(lk);
  }


  p->chan = chan;
  if(p->chan2 == 0)
    p->chan2 = chan2;
  p->state = SLEEPING;



  sched();


  p->chan = 0;


  if(lk != &p->lock){
    release(&p->lock);
    acquire(lk);
  }
}

int
wakeup2(void *chan, int n, void *chan2, int m)
{
  struct proc *p;
  int count1 = 0, count2 = 0;



  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);

    if(p->state == SLEEPING && p->chan2 == chan){
      if(count1 < n){

        p->state = RUNNABLE;

        count1++;
      }
      else if(chan2 && count2 < m){

        p->chan2 = chan2;
        count2++;
      }

      if(count1 >= n && count2 >= m){
        release(&p->lock);
        break;
      }
    }
    release(&p->lock);
  }

  return count1;
}
