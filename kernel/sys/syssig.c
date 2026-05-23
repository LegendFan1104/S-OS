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

uint64 sys_sigaltstack(void) {
    uint64 ss_addr, old_ss_addr;
    argaddr(0, &ss_addr);
    argaddr(1, &old_ss_addr);
    
    struct proc *p = myproc();
    struct sigaltstack ss, old_ss;
    
    if (ss_addr) {
        if (copyin(p->pagetable, (char*)&ss, ss_addr, sizeof(ss)) < 0)
            return -14;  // -EFAULT
    }
    
    int ret = sigaltstack_impl(p, ss_addr ? &ss : NULL, old_ss_addr ? &old_ss : NULL);
    if (ret < 0)
        return ret;
    
    if (old_ss_addr) {
        if (copyout(p->pagetable, old_ss_addr, (char*)&old_ss, sizeof(old_ss)) < 0)
            return -14;  // -EFAULT
    }
    
    return 0;
}

uint64 sys_sigsuspend(void) {
    uint64 mask_addr;
    argaddr(0, &mask_addr);
    
    struct proc *p = myproc();
    sigset_t mask;
    
    if (copyin(p->pagetable, (char*)&mask, mask_addr, sizeof(mask)) < 0)
        return -14;  // -EFAULT
    
    return sigsuspend_impl(p, &mask);
}

uint64 sys_rt_sigpending(void) {
    uint64 set_addr;
    argaddr(0, &set_addr);
    
    struct proc *p = myproc();
    sigset_t pending;
    
    // Get pending signals (simplified - in real impl would check signal queue)
    // p->signal is an int representing the pending signal number
    // Convert it to a sigset_t
    pending.val = 0;
    if (p->signal > 0 && p->signal <= 64) {
        pending.val = (1UL << (p->signal - 1));
    }
    
    if (copyout(p->pagetable, set_addr, (char*)&pending, sizeof(pending)) < 0)
        return -14;  // -EFAULT
    
    return 0;
}









