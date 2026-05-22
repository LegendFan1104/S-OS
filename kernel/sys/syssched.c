#include "types.h"
#include "platform.h"
#include "defs.h"
#include "param.h"
#include "mem/memlayout.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "lib/string.h"
#include "sys/syscall.h"

// 用户空间的sched_param结构体
// 注意：这个结构体定义在libc中，这里使用相同的布局
struct user_sched_param {
    int sched_priority;
};

// 外部声明proc数组
extern struct proc proc[];

// sys_sched_setscheduler - set scheduling policy and parameters
uint64 sys_sched_setscheduler(void) {
    int pid, policy;
    uint64 param_addr;
    struct user_sched_param param;
    struct proc *p;
    
    argint(0, &pid);
    argint(1, &policy);
    argaddr(2, &param_addr);
    
    if (param_addr != 0) {
        if (copyin(myproc()->pagetable, (char *)&param, param_addr, sizeof(param)) < 0)
            return -1;
    } else {
        param.sched_priority = 0;
    }
    
    // Validate policy
    if (policy != SCHED_OTHER && policy != SCHED_FIFO && 
        policy != SCHED_RR && policy != SCHED_BATCH && policy != SCHED_IDLE)
        return -1;
    
    // Validate priority for real-time policies
    if ((policy == SCHED_FIFO || policy == SCHED_RR) && 
        (param.sched_priority < SCHED_MIN_PRIORITY || param.sched_priority > SCHED_MAX_PRIORITY))
        return -1;
    
    // Find target process
    if (pid == 0) {
        p = myproc();
        acquire(&p->lock);
    } else {
        p = 0;
        for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
            acquire(&pp->lock);
            if (pp->pid == pid) {
                p = pp;
                break;
            }
            release(&pp->lock);
        }
        if (p == 0)
            return -1;
    }
    
    p->sched_policy = policy;
    p->sched_priority = param.sched_priority;
    
    release(&p->lock);
    return 0;
}

// sys_sched_setparam - set scheduling parameters
uint64 sys_sched_setparam(void) {
    int pid;
    uint64 param_addr;
    struct user_sched_param param;
    struct proc *p;
    
    argint(0, &pid);
    argaddr(1, &param_addr);
    
    if (copyin(myproc()->pagetable, (char *)&param, param_addr, sizeof(param)) < 0)
        return -1;
    
    // Find target process
    if (pid == 0) {
        p = myproc();
        acquire(&p->lock);
    } else {
        p = 0;
        for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
            acquire(&pp->lock);
            if (pp->pid == pid) {
                p = pp;
                break;
            }
            release(&pp->lock);
        }
        if (p == 0)
            return -1;
    }
    
    // Validate priority for real-time policies
    if ((p->sched_policy == SCHED_FIFO || p->sched_policy == SCHED_RR) && 
        (param.sched_priority < SCHED_MIN_PRIORITY || param.sched_priority > SCHED_MAX_PRIORITY)) {
        release(&p->lock);
        return -1;
    }
    
    p->sched_priority = param.sched_priority;
    
    release(&p->lock);
    return 0;
}

// sys_sched_getparam - get scheduling parameters
uint64 sys_sched_getparam(void) {
    int pid;
    uint64 param_addr;
    struct user_sched_param param;
    struct proc *p;
    
    argint(0, &pid);
    argaddr(1, &param_addr);
    
    // Find target process
    if (pid == 0) {
        p = myproc();
        acquire(&p->lock);
    } else {
        p = 0;
        for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
            acquire(&pp->lock);
            if (pp->pid == pid) {
                p = pp;
                break;
            }
            release(&pp->lock);
        }
        if (p == 0)
            return -1;
    }
    
    param.sched_priority = p->sched_priority;
    
    release(&p->lock);
    
    if (copyout(myproc()->pagetable, param_addr, (char *)&param, sizeof(param)) < 0)
        return -1;
    
    return 0;
}

// sys_sched_getscheduler - get scheduling policy
uint64 sys_sched_getscheduler(void) {
    int pid;
    struct proc *p;
    int policy;
    
    argint(0, &pid);
    
    // Find target process
    if (pid == 0) {
        p = myproc();
        acquire(&p->lock);
    } else {
        p = 0;
        for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
            acquire(&pp->lock);
            if (pp->pid == pid) {
                p = pp;
                break;
            }
            release(&pp->lock);
        }
        if (p == 0)
            return -1;
    }
    
    policy = p->sched_policy;
    
    release(&p->lock);
    return policy;
}

// sys_sched_get_priority_max - get maximum priority for a policy
uint64 sys_sched_get_priority_max(void) {
    int policy;
    
    argint(0, &policy);
    
    switch (policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        return SCHED_MAX_PRIORITY;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
        return 0;
    default:
        return -1;
    }
}

// sys_sched_get_priority_min - get minimum priority for a policy
uint64 sys_sched_get_priority_min(void) {
    int policy;
    
    argint(0, &policy);
    
    switch (policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        return SCHED_MIN_PRIORITY;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
        return 0;
    default:
        return -1;
    }
}

// sys_sched_setaffinity - set CPU affinity
uint64 sys_sched_setaffinity(void) {
    int pid;
    uint64 len, mask_addr;
    uint64 mask;
    struct proc *p;
    
    argint(0, &pid);
    argaddr(1, &len);
    argaddr(2, &mask_addr);
    
    if (len < sizeof(uint64))
        return -1;
    
    if (copyin(myproc()->pagetable, (char *)&mask, mask_addr, sizeof(uint64)) < 0)
        return -1;
    
    // Find target process
    if (pid == 0) {
        p = myproc();
        acquire(&p->lock);
    } else {
        p = 0;
        for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
            acquire(&pp->lock);
            if (pp->pid == pid) {
                p = pp;
                break;
            }
            release(&pp->lock);
        }
        if (p == 0)
            return -1;
    }
    
    p->cpu_affinity = mask;
    
    release(&p->lock);
    return 0;
}

// sys_sched_getaffinity - get CPU affinity
uint64 sys_sched_getaffinity(void) {
    int pid;
    uint64 len, mask_addr;
    uint64 mask;
    struct proc *p;
    
    argint(0, &pid);
    argaddr(1, &len);
    argaddr(2, &mask_addr);
    
    if (len < sizeof(uint64))
        return -1;
    
    // Find target process
    if (pid == 0) {
        p = myproc();
        acquire(&p->lock);
    } else {
        p = 0;
        for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
            acquire(&pp->lock);
            if (pp->pid == pid) {
                p = pp;
                break;
            }
            release(&pp->lock);
        }
        if (p == 0)
            return -1;
    }
    
    mask = p->cpu_affinity;
    
    release(&p->lock);
    
    if (copyout(myproc()->pagetable, mask_addr, (char *)&mask, sizeof(uint64)) < 0)
        return -1;
    
    return sizeof(uint64);
}
