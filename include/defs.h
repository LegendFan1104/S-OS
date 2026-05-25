#pragma once
#include "types.h"
#include "platform.h"

struct buf;
struct context;
struct file;
struct inode;
struct pipe;
struct proc;
struct spinlock;
struct sleeplock;
struct stat;
struct superblock;

// bio.c


// console.c
void            consoleinit(void);
void            consoleintr(int);
void            consputc(int);
int             consoleready(void);


// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);
int             pipewrite_kernel(struct pipe *pi, uint64 addr, int n);
int             piperead_kernel(struct pipe *pi, char* addr, int n);

// printf.c
void            panic(char*) __attribute__((noreturn));
void            printfinit(void);

// proc.c
int             cpuid(void);
void            exit(int);
int             fork(void);
int             growproc(int64);
void            proc_mapstacks(pagetable_t);
pagetable_t     proc_pagetable(struct proc *);
void            proc_freepagetable(pagetable_t, uint64);
int             kill(int);
int             killed(struct proc*);
void            setkilled(struct proc*);
struct cpu*     mycpu(void);
struct cpu*     getmycpu(void);
struct proc*    myproc();
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            sched(void);
void            sleep(void*, struct spinlock*);
void            userinit(void);
int             wait(uint64);
void            wakeup(void*);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);
int            tkill(int, int);
int            kill_signal(int, int);
uint64          countprocs(void);
int read_interrupts(char *context);
void receive_interrupts(int irq);
void  sleep1(void *chan, void *chan2, struct spinlock *lk);
int  wakeup2(void *chan, int n, void *chan2, int m);


// swtch.S
void            swtch(struct context*, struct context*);

// spinlock.c
void            acquire(struct spinlock*);
int             holding(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            release(struct spinlock*);
void            push_off(void);
void            pop_off(void);

// sleeplock.c
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            initsleeplock(struct sleeplock*, char*);


// syscall.c
void            argint(int, int*);
int             argstr(int, char*, int);
void            argaddr(int, uint64 *);
int             fetchstr(uint64, char*, int);
int             fetchaddr(uint64, uint64*);
void            syscall();

// trap.c
extern uint     ticks;
void            trapinit(void);
void            trapinithart(void);
extern struct spinlock tickslock;
void            usertrapret(void);
struct trapframe;
void            trapframedump(struct trapframe *tf);
int             pagefault_handler(uint64 va, uint64 cause);

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartputc(int);
void            uartputc_sync(int);
int             uartgetc(void);

// vm.c
void            kvminit(void);
void            kvminithart(void);
void            kvmmap(pagetable_t, uint64, uint64, uint64, uint64);
int             mappages(pagetable_t, uint64, uint64, uint64, uint64);
int protectpages(pagetable_t pagetable, uint64 va, uint64 size, int perm);
pagetable_t     uvmcreate(void);
void            uvmfirst(pagetable_t, uchar *, uint);
uint64          uvmalloc(pagetable_t, uint64, uint64, int);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
int             uvmcopy(pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
pte_t *         walk(pagetable_t, uint64, int);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
void kernelmap(uint64 va, uint64 pa, uint64 sz, uint64 perm);
uint64          kwalkaddr(uint64 va);
void pci_map(int bus, int dev, int func, void *pages);

uint64 sys_shutdown(void);



// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))


// add
int wait4(int, int*, int);
int clone(unsigned long flags, void *stack, int *ptid, unsigned long tls, int *ctid);

#define CLONE_VM     0x00000100  // 共享内存空间
#define CLONE_FS     0x00000200  // 共享文件系统信息
#define CLONE_FILES  0x00000400  // 共享文件描述符表
#define CLONE_PARENT 0x00000800  // 共享父进程

uint64 getppid();

// New LTP-critical syscalls
uint64 sys_prctl(void);
uint64 sys_getrlimit(void);
uint64 sys_setrlimit(void);
uint64 sys_getrusage(void);
uint64 sys_getpriority(void);
uint64 sys_setpriority(void);
uint64 sys_umask(void);
uint64 sys_statfs(void);
uint64 sys_fstatfs(void);
uint64 sys_sync(void);
uint64 sys_fsync(void);
uint64 sys_fdatasync(void);
uint64 sys_rt_sigpending(void);
uint64 sys_rt_sigsuspend(void);
uint64 sys_sigaltstack(void);
uint64 sys_fchownat(void);
uint64 sys_mknodat(void);
uint64 sys_personality(void);
uint64 sys_setresuid(void);
uint64 sys_getresuid(void);
uint64 sys_setresgid(void);
uint64 sys_getresgid(void);
uint64 sys_getgroups(void);
uint64 sys_setgroups(void);
