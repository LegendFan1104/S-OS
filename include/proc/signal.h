#pragma once
#include "types.h"
#include "lib/list.h"
#include "proc/trapframe.h"

#define SIGNUM 64
#define NSIG   64

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGIOT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL SIGIO
#define SIGPWR 30
#define SIGSYS 31
#define SIGUNUSED 31

// Signal flags for sigaction
#define SA_NOCLDSTOP    0x00000001
#define SA_NOCLDWAIT    0x00000002
#define SA_SIGINFO      0x00000004
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000

// Flags for sigprocmask
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// Alternate signal stack flags
#define SS_ONSTACK      1
#define SS_DISABLE      2
#define MINSIGSTKSZ     2048
#define SIGSTKSZ        8192

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t) 0) /* default signal handling */
#define SIG_IGN ((sighandler_t) 1) /* ignore signal */
#define SIG_ERR ((sighandler_t) - 1) /* error return from signal */

//使用掩码来记录信号
struct sigset{
    uint64 val;
};

typedef struct sigset sigset_t;

// Alternate signal stack structure
struct sigaltstack {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
};
typedef struct sigaltstack stack_t;

// siginfo_t for waitid
struct siginfo {
    int si_signo;
    int si_code;
    int si_pid;
    int si_uid;
    int si_status;
    int si_utime;
    int si_stime;
};
typedef struct siginfo siginfo_t;

// idtype_t for waitid
typedef enum {
    P_ALL = 0,
    P_PID = 1,
    P_PGID = 2,
    P_PIDFD = 3
} idtype_t;

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

struct signal_frame {
    sigset_t mask;
    struct trapframe tf;
    struct signal_frame *next;
};

// signal process
struct sighand {
    struct sigaction action[SIGNUM + 1];
};

#define is_valid(sig) (((sig) <= SIGNUM && (sig) >= 1) ? 1 : 0)
#define sig_is_member(set, n_sig) (1 & (set.val >> (n_sig - 1)))
#define is_ignored(sig) (sig_is_member(p->block, sig))

struct proc;

void handle_signal();
void do_handle(struct proc *p, int signum, struct sigaction *act);
void default_handle(struct proc *p, int signum);
void sig_return();
int sigprocmask(int how, sigset_t *set, sigset_t *oldset);
int sigact_reg(int signum, struct sigaction *act, struct sigaction *oldact);
int sigaltstack_impl(struct proc *p, struct sigaltstack *ss, struct sigaltstack *old_ss);
int sigsuspend_impl(struct proc *p, sigset_t *mask);














