#pragma once

#include "fs/vfs/file.h"
#include "time.h"
#include "proc/signal.h"
#include "proc/trapframe.h"

// Scheduling policies
#define SCHED_OTHER     0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5

// Priority limits
#define SCHED_MIN_PRIORITY      1
#define SCHED_MAX_PRIORITY      99
#define SCHED_DEFAULT_PRIORITY  0

// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };


#define NVMA 16
// 虚拟内存区域结构体
struct vm_area {
  int used;           // 是否已被使用
  uint64 addr;        // 起始地址
  int len;            // 长度
  int prot;           // 权限
  int flags;          // 标志位
  int vfd;            // 对应的文件描述符
  struct file* vfile; // 对应文件
  int offset;         // 文件偏移，本实验中一直为0
};

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID
  
  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct file_vnode cwd;           // Current directory 因为暂时用file结构来代表目录，所以这里这样实现
  char name[16];               // Process name (debugging)

  //mmap
  struct vm_area vma[NVMA];
  struct tms proc_tms;

  //signal
  sigset_t block;
  int signal; // 等待的信号
  struct sighand *sig; // 信号处理相关
  struct signal_frame *sig_frame;
  struct sigaltstack sigaltstack; // Alternate signal stack
  sigset_t sigsuspend_mask;       // Saved mask for sigsuspend
  int sigsuspend_active;          // Flag for sigsuspend state
  
  // Robust futex list
  struct robust_list_head *robust_list;
  size_t robust_list_len;

  void *chan2;                // Used for futex

  //用于set_tid_address
  uint64 clear_child_tid;

  int uid;  // 用户ID
  int gid;  // 组ID
  int pgid; // 进程组ID

  // 调度相关
  int sched_policy;           // 调度策略: SCHED_OTHER, SCHED_FIFO, SCHED_RR
  int sched_priority;         // 调度优先级 (1-99 for RT)
  uint64 cpu_affinity;        // CPU亲和性掩码
};

#define WNOHANG     0x01    // Don't block waiting
#define WUNTRACED   0x02    // Report stopped children
#define WCONTINUED  0x04    // Report continued children
#define WNOWAIT     0x08    // Don't reap, just poll status

// Siginfo codes for SIGCHLD
#define CLD_EXITED  1       // Child has exited
#define CLD_KILLED  2       // Child was killed
#define CLD_DUMPED  3       // Child terminated abnormally
#define CLD_TRAPPED 4       // Traced child has trapped
#define CLD_STOPPED 5       // Child has stopped
#define CLD_CONTINUED 6     // Stopped child has continued

#define S_ISUID  04000  // set-user-ID bit
#define S_ISGID  02000  // set-group-ID bit

#define ROOT_UID      0    // 超级用户
#define DEFAULT_UID   0    // 普通用户默认UID
#define ROOT_GID      0
#define DEFAULT_GID   1000

struct sysinfo{
    uint64 uptime; //系统运行时间
    // uint64 loads[3]; //系统负载
    // uint64 totalram; //总内存
    uint64 freeram; //空闲内存
    // uint64 sharedram; //共享内存
    // uint64 bufferram; //缓存内存
    // uint64 totalswap; //交换区总大小
    // uint64 freeswap; //交换区剩余大小
    uint64 procs; //进程数
};



