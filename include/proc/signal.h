#pragma once
#include "types.h"
#include "lib/list.h"
#include "proc/trapframe.h"

#define SIGNUM 64

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



// Flags for sigprocmask
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t) 0) /* default signal handling */
#define SIG_IGN ((sighandler_t) 1) /* ignore signal */
#define SIG_ERR ((sighandler_t) - 1) /* error return from signal */

//使用掩码来记录信号
struct sigset{
    uint64 val;
};

typedef struct sigset sigset_t;



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














