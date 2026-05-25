#include "types.h"
#include "platform.h"
#include "defs.h"
#include "param.h"
#include "mem/memlayout.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "mem/mem.h"
#include "proc/signal.h"

uint64 sys_rt_sigaction(void) {
    int signum;
    uint64 actaddr, oactaddr;

    argint(0, &signum);
    argaddr(1, &actaddr);
    argaddr(2, &oactaddr);

    struct sigaction act={0}, oact={0};
    struct proc *p = myproc();

    if (actaddr) {
        if (copyin(p->pagetable, (char*)&act, actaddr, sizeof(act)) < 0) {
            return -1;
        }
    }

    if(sigact_reg(signum, actaddr ? &act : NULL, oactaddr ? &oact : NULL) < 0)
        return -1;

    if(oactaddr){
        if (copyout(p->pagetable, oactaddr, (char*)&oact, sizeof(oact)) < 0)
            return -1;
    }

    return 0;
}

uint64 sys_rt_sigprocmask(void) {
    int how;
    uint64 setaddr, oldaddr;
    sigset_t set, oldset;
    argint(0, &how);
    argaddr(1, &setaddr);
    argaddr(2, &oldaddr);

    // printf("%d %p %p\n", how, setaddr, oldaddr);

    struct proc *p = myproc();
    if (setaddr) {
        if (copyin(p->pagetable, (char*)&set, setaddr, sizeof(set)) < 0) {
            return -1;
        }
    }

    if (sigprocmask(how, &set, oldaddr ? &oldset : NULL)) {
        return -1;
    }

    if (oldaddr) {
        if (copyout(p->pagetable, oldaddr, (char*)&oldset, sizeof(oldset)) < 0) {
            return -1;
        }
    }
    return 0;
}

uint64
sys_rt_sigtimedwait(void)
{
    return 0;
}

uint64 sys_kill_signal(void) {
    int pid, sig;
    argint(0, &pid);
    argint(1, &sig);
    return kill_signal(pid, sig);
}

uint64 sys_tkill(void) {
    int tid, sig;
    argint(0, &tid);
    argint(1, &sig);
    return tkill(tid, sig);
}

uint64 sys_tgkill(void) {
    return 0;
}

// rt_sigpending - get set of pending signals
uint64 sys_rt_sigpending(void) {
    uint64 set_addr;
    argaddr(0, &set_addr);
    struct proc *p = myproc();
    sigset_t pending = p->pending;
    if (copyout(p->pagetable, set_addr, (char*)&pending, sizeof(sigset_t)) < 0)
        return -1;
    return 0;
}

// rt_sigsuspend - wait for signals
uint64 sys_rt_sigsuspend(void) {
    uint64 mask_addr;
    argaddr(0, &mask_addr);
    struct proc *p = myproc();
    sigset_t mask, oldmask;

    if (copyin(p->pagetable, (char*)&mask, mask_addr, sizeof(sigset_t)) < 0)
        return -1;

    oldmask = p->block;
    p->block = mask;

    // Wait until a signal arrives
    while (p->pending.val == 0) {
        if (killed(p)) {
            p->block = oldmask;
            return -4;  // EINTR
        }
        sleep(p, &p->lock);
    }

    p->block = oldmask;
    return -4;  // Always return EINTR after signal
}

// sigaltstack - set/get alternate signal stack
uint64 sys_sigaltstack(void) {
    uint64 ss_addr, old_ss_addr;
    argaddr(0, &ss_addr);
    argaddr(1, &old_ss_addr);
    struct proc *p = myproc();

    if (old_ss_addr) {
        if (copyout(p->pagetable, old_ss_addr, (char*)&p->altstack, sizeof(p->altstack)) < 0)
            return -1;
    }
    if (ss_addr) {
        if (copyin(p->pagetable, (char*)&p->altstack, ss_addr, sizeof(p->altstack)) < 0)
            return -1;
    }
    return 0;
}









