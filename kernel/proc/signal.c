#include "proc/signal.h"
#include "proc/proc.h"
#include <mem/memlayout.h>
#include "lib/string.h"


extern char sig_trampoline[];
#ifdef RISCV
extern char sig_handler[];
#endif

//注册信号处理程序
int sigact_reg(int signum, struct sigaction *act, struct sigaction *oldact)
{
    struct proc *p = myproc();

    if(signum < 1 || signum > SIGNUM)
        return -1;

    if(signum == SIGKILL || signum == SIGSTOP)
        return -1;

    if(oldact)
        *oldact = (p->sig)->action[signum];

    if(act) {
        (p->sig)->action[signum] = *act;
        // printf("reg:%p\n", act->sa_handler);
    }


    return 0;
}

int sigprocmask(int how, sigset_t *set, sigset_t *oldset)
{
    struct proc *p = myproc();

    if(oldset)
        oldset->val = p->block.val;

    switch(how)
    {
        case SIG_BLOCK:
            p->block.val |= set->val;
        break;
        case SIG_UNBLOCK:
            p->block.val &= ~(set->val);
        break;
        case SIG_SETMASK:
            p->block.val = set->val;
        break;
        default:
            panic("[sigprocmask] invalid how\n");
    }
    // p->block.val |= 1UL << SIGKILL | 1UL << SIGSTOP;

    return 0;
}

void default_handle(struct proc *p, int signum) {
    uint64 wstatus = 0;
    switch (signum) {
        case SIGKILL:
            p->killed = 1;
            break;
        case SIGCHLD:
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
        case SIGCONT:
            break;
        default:
            p->killed = 1;
            break;
    }
}


void handle_signal() {
    struct proc *p = myproc();
    if (p->signal == 0) {
        return;
    }

    int signum = p->signal;

    struct sigaction *act = &((p->sig)->action[signum]);
    if (!is_valid(signum)) {
        panic("Invalid signal");
    }
    if (is_ignored(signum)) {
        return ;
    }
    if (act->sa_handler == SIG_DFL) {
        default_handle(p, signum);
        p->signal = 0;
    } else if (act->sa_handler == SIG_IGN) {
        return ;
    } else {
        do_handle(p, signum, act);
        p->signal = 0;
    }

    return ;
}


/*
 *可能会出现的问题
 *当信号处理函数地址为0的时候会被默认处理，由于OS内核对于用户进程链接的原因可能导致
 */
void do_handle(struct proc *p, int signum, struct sigaction *act) {
    struct signal_frame *frame;
    frame= kmalloc(sizeof(struct signal_frame));
    frame -> mask.val = p -> block.val;
    if (act == NULL) {
        p->block.val = 0;
    } else {
        p->block.val = act -> sa_mask.val;
    }

    if (p->block.val & 1UL << signum) {
        kfree(frame);
        return ;
    }



    frame->tf =  *(p->trapframe);
    // printf("aaa:  %p %p %p %p\n", sig_trampoline, SIG_TRAMPOLINE, sig_handler, walk(p->pagetable, (uint64)(SIG_TRAMPOLINE), 0));
#ifdef RISCV
    p->trapframe->ra = (uint64)(SIG_TRAMPOLINE + ((uint64)sig_handler - (uint64)sig_trampoline));
#elif defined(LOONGARCH)
    p->trapframe->ra = (uint64)SIG_TRAMPOLINE;
    printf("sig: %p\n", SIG_TRAMPOLINE);
#endif
    // printf("ra: %x   %x\n", *(uint64 *)(p->trapframe->ra), *(uint64 *)(p->trapframe->ra + 8));
    p->trapframe->sp -= PGSIZE;
    p->trapframe->a0 = signum;
#ifdef RISCV
    p->trapframe->epc = (uint64)(act->sa_handler);
#elif defined(LOONGARCH)
    p->trapframe->era = (uint64)(act->sa_handler);
#endif

    if (p->sig_frame) {
        frame->next = p->sig_frame->next;
    } else {
        frame->next = NULL;
    }
    p->sig_frame = frame;

    return ;
}

void sig_return() {
    struct proc *p = myproc();
    if (p->sig_frame == NULL) {
        p->killed = 1;
        return ;
    }
    struct signal_frame *frame = p->sig_frame;

    p->block.val = frame->mask.val;
    memcpy(p->trapframe, &(frame->tf), sizeof(struct trapframe));
    // trapframedump(p->trapframe);
    p->sig_frame = frame->next;

    kfree(frame);
}

// sigaltstack implementation
int sigaltstack_impl(struct proc *p, struct sigaltstack *ss, struct sigaltstack *old_ss) {
    if (!p)
        return -1;
    
    // Return current alt-stack info if requested
    if (old_ss) {
        old_ss->ss_sp = p->sigaltstack.ss_sp;
        old_ss->ss_size = p->sigaltstack.ss_size;
        old_ss->ss_flags = p->sigaltstack.ss_flags;
    }
    
    // Set new alt-stack if requested
    if (ss) {
        // Check if currently on alt-stack
        if (p->sigaltstack.ss_flags & SS_ONSTACK)
            return -1;  // Cannot modify while on stack
        
        // Validate flags
        if (ss->ss_flags & ~SS_DISABLE)
            return -1;  // Invalid flags
        
        if (ss->ss_flags & SS_DISABLE) {
            // Disable alt-stack
            p->sigaltstack.ss_sp = NULL;
            p->sigaltstack.ss_size = 0;
            p->sigaltstack.ss_flags = SS_DISABLE;
        } else {
            // Enable alt-stack
            if (ss->ss_size < MINSIGSTKSZ)
                return -1;  // Stack too small
            
            p->sigaltstack.ss_sp = ss->ss_sp;
            p->sigaltstack.ss_size = ss->ss_size;
            p->sigaltstack.ss_flags = 0;
        }
    }
    
    return 0;
}

// sigsuspend implementation
int sigsuspend_impl(struct proc *p, sigset_t *mask) {
    if (!p || !mask)
        return -1;
    
    // Save old mask
    p->sigsuspend_mask.val = p->block.val;
    p->sigsuspend_active = 1;
    
    // Set new mask (can't block SIGKILL or SIGSTOP)
    uint64 new_mask = mask->val;
    new_mask &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
    p->block.val = new_mask;
    
    // Check if any signal is already pending and unblocked
    // If so, return immediately without sleeping
    if (p->signal != 0 && !(p->block.val & (1UL << (p->signal - 1)))) {
        p->sigsuspend_active = 0;
        return -4;  // EINTR
    }
    
    // Sleep until signal arrives
    while (p->sigsuspend_active) {
        // Check for pending signals
        if (p->signal != 0 && !(p->block.val & (1UL << (p->signal - 1)))) {
            break;
        }
        
        // Check if killed
        if (p->killed) {
            break;
        }
        
        yield();
    }
    
    // Restore old mask (done in signal handler or here if interrupted)
    if (p->sigsuspend_active) {
        p->block.val = p->sigsuspend_mask.val;
        p->sigsuspend_active = 0;
    }
    
    // Always return -EINTR
    return -4;  // EINTR
}

















