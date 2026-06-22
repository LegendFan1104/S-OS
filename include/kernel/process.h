#ifndef __PROCESS_H__
#define __PROCESS_H__

#include "types.h"
#include "spinlock.h"
#include "trap.h"
#include "fs_defs.h"
#include "vma.h"
#include "file.h"
#include "timer.h"
#include "signal.h"
#include "context.h"
#include "thread.h"
#include "list.h"
#include "resource.h"

#define NPROC (256)

/* Cloning flags.  */
#define CSIGNAL 0x000000ff
#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_PIDFD 0x00001000
#define CLONE_PTRACE 0x00002000
#define CLONE_VFORK 0x00004000
#define CLONE_PARENT 0x00008000
#define CLONE_THREAD 0x00010000
#define CLONE_NEWNS 0x00020000
#define CLONE_SYSVSEM 0x00040000
#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED 0x00400000
#define CLONE_UNTRACED 0x00800000
#define CLONE_CHILD_SETTID 0x01000000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS 0x04000000
#define CLONE_NEWIPC 0x08000000
#define CLONE_NEWUSER 0x10000000
#define CLONE_NEWPID 0x20000000
#define CLONE_NEWNET 0x40000000
#define CLONE_IO 0x80000000

enum procstate
{
    UNUSED,
    USED,
    SLEEPING,
    RUNNABLE,
    RUNNING,
    ZOMBIE
};

typedef struct thread thread_t;

#define MAX_SHAREMEMORY_REGION_NUM 20

typedef struct proc
{
    spinlock_t lock;
    void *chan;
    struct proc *parent;

    thread_t *main_thread;
    struct list thread_queue;

    enum procstate state;
    int exit_state;
    int killed;
    int term_signal;
    int pid;
    int pgid;
    int sid;
    int uid;
    int gid;
    uint32 umask;
    int oom_score_adj;
    uint64 virt_addr;
    uint64 sz;
    uint64 kstack;
    struct trapframe *trapframe;
    struct context context;
    pgtbl_t pagetable;

    int utime;
    int ktime;
    int thread_num;
    uint64 clear_child_tid;
    struct vma *vma;
    struct sharememory *sharememory[MAX_SHAREMEMORY_REGION_NUM];
    int shm_num;
    uint64 shm_size;
    struct itimerval itimer;
    uint64 alarm_ticks;
    int timer_active;

    struct file *ofile[NOFILE];
    struct file_vnode cwd;
    struct rlimit ofn;

    __sigset_t sig_set;
    sigaction sigaction[SIGRTMAX + 1];
    __sigset_t sig_pending;
} proc_t;

typedef struct start_args_t
{
    uint64_t start_func;
    uint64_t arg;
    uint64_t sig_mask[8];
    uint64_t control;
} args_t;

void copytrapframe(struct trapframe *dest, struct trapframe *src);
void proc_init();
struct proc *getproc(int pid);
void scheduler() __attribute__((noreturn));
struct proc *allocproc();
pgtbl_t proc_pagetable(struct proc *p);
void proc_freepagetable(struct proc *p, uint64 sz);
void proc_mapstacks(pgtbl_t pagetable);
void sleep_on_chan(void *, struct spinlock *);
void wakeup(void *);
void yield(void);
uint64 fork(void);
int clone(uint64 flags, uint64 stack, uint64 ptid, uint64 ctid);
int wait(int pid, uint64 addr, int options);
void exit(int exit_state);
void proc_yield(void);
void reg_info(void);
int growproc(int n);
int exec(char *path, char **argv, char **env);
int killed(struct proc *p);
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void procdump(void);
uint64 procnum(void);
int proc_get_oom_score_adj(int pid, int *value);
int proc_set_oom_score_adj(int pid, int value);
int kill(int pid, int sig);
int kill_all(int sig, proc_t *exclude);
int tgkill(int tgid, int tid, int sig);
void sched(void);
uint64 clone_thread(uint64 stack_va, uint64 ptid, uint64 tls, uint64 ctid, uint64 flags);

#endif
