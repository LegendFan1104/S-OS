#include "types.h"
#include "param.h"
#include "mem/memlayout.h"
#include "platform.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "proc/futex.h"
#include "defs.h"
#include "lib/string.h"
#include "mem/mem.h"
#include "time.h"

static struct futex_waiter futex_waiters[MAX_FUTEX_WAITERS];
static struct spinlock futex_lock;

void futex_init(void) {
    initlock(&futex_lock, "futex");
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        futex_waiters[i].active = 0;
        futex_waiters[i].woken = 0;
        initlock(&futex_waiters[i].lock, "futex_waiter");
    }
}

// Get physical address for futex matching
uint64 futex_get_physical_addr(uint64 uaddr) {
    struct proc *p = myproc();
    if (!p || !p->pagetable)
        return 0;
    
    pte_t *pte = walk(p->pagetable, uaddr, 0);
    if (!pte || !(*pte & PTE_V))
        return 0;
    
    uint64 pa = PTE2PA(*pte);
    return pa + (uaddr & (PGSIZE - 1));
}

// Clear all waiters for a process (called on exit)
void futex_clear_waiter(struct proc *p) {
    if (!p)
        return;
    
    acquire(&futex_lock);
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].active && futex_waiters[i].p == p) {
            futex_waiters[i].active = 0;
            futex_waiters[i].woken = 0;
            futex_waiters[i].p = NULL;
        }
    }
    release(&futex_lock);
}

// Allocate a futex waiter slot
static int futex_alloc_waiter(struct proc *p, uint64 uaddr, uint32_t bitset) {
    acquire(&futex_lock);
    
    // Clear any existing waiter for this process
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].active && futex_waiters[i].p == p) {
            futex_waiters[i].active = 0;
        }
    }
    
    // Find a free slot
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (!futex_waiters[i].active) {
            futex_waiters[i].active = 1;
            futex_waiters[i].woken = 0;
            futex_waiters[i].uaddr = uaddr;
            futex_waiters[i].paddr = futex_get_physical_addr(uaddr);
            futex_waiters[i].p = p;
            futex_waiters[i].bitset = bitset;
            release(&futex_lock);
            return i;
        }
    }
    
    release(&futex_lock);
    return -1;  // No free slots
}

// Mark a waiter as woken
static void futex_wake_waiter(int idx) {
    if (idx < 0 || idx >= MAX_FUTEX_WAITERS)
        return;
    
    acquire(&futex_waiters[idx].lock);
    if (futex_waiters[idx].active) {
        futex_waiters[idx].woken = 1;
        if (futex_waiters[idx].p && futex_waiters[idx].p->state == SLEEPING) {
            futex_waiters[idx].p->chan2 = 0;  // Clear chan2 to indicate wakeup
        }
    }
    release(&futex_waiters[idx].lock);
}

// FUTEX_WAIT operation
int futex_wait(uint64 uaddr, int val, uint64 timeout_addr, int clockrt, uint32_t bitset) {
    struct proc *p = myproc();
    if (!p)
        return -1;
    
    // Check alignment
    if (uaddr % sizeof(int) != 0)
        return -22;  // -EINVAL
    
    // Read current futex value
    int futex_val;
    if (copyin(p->pagetable, (char*)&futex_val, uaddr, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    // Value mismatch
    if (futex_val != val)
        return -11;  // -EAGAIN (was -EWOULDBLOCK)
    
    // Allocate waiter slot
    int waiter_idx = futex_alloc_waiter(p, uaddr, bitset);
    if (waiter_idx < 0)
        return -12;  // -ENOMEM
    
    // Handle timeout
    uint64 timeout_ticks = 0;
    if (timeout_addr) {
        struct timespec ts;
        if (copyin(p->pagetable, (char*)&ts, timeout_addr, sizeof(ts)) < 0) {
            futex_waiters[waiter_idx].active = 0;
            return -14;  // -EFAULT
        }
        
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
            futex_waiters[waiter_idx].active = 0;
            return -22;  // -EINVAL
        }
        
        // Convert to ticks
        timeout_ticks = ts.tv_sec * 100 + (ts.tv_nsec * 100) / 1000000000L;
        if (timeout_ticks == 0 && (ts.tv_sec > 0 || ts.tv_nsec > 0))
            timeout_ticks = 1;
    }
    
    // Sleep on the futex
    acquire(&p->lock);
    
    // Check if already woken
    if (futex_waiters[waiter_idx].woken) {
        release(&p->lock);
        futex_waiters[waiter_idx].active = 0;
        return 0;
    }
    
    p->chan = (void*)uaddr;
    p->chan2 = (void*)uaddr;
    p->state = SLEEPING;
    
    uint64 start_tick = ticks;
    
    while (p->state == SLEEPING && !futex_waiters[waiter_idx].woken) {
        // Check for timeout
        if (timeout_ticks > 0 && (ticks - start_tick) >= timeout_ticks) {
            p->state = RUNNABLE;
            release(&p->lock);
            futex_waiters[waiter_idx].active = 0;
            return -110;  // -ETIMEDOUT
        }
        
        // Check for signals
        if (p->killed || (p->signal != 0 && !(p->block.val & (1UL << p->signal)))) {
            p->state = RUNNABLE;
            release(&p->lock);
            futex_waiters[waiter_idx].active = 0;
            return -512;  // -ERESTARTSYS
        }
        
        release(&p->lock);
        yield();
        acquire(&p->lock);
    }
    
    p->chan = 0;
    p->chan2 = 0;
    release(&p->lock);
    
    int was_woken = futex_waiters[waiter_idx].woken;
    futex_waiters[waiter_idx].active = 0;
    
    if (!was_woken && p->killed)
        return -512;  // -ERESTARTSYS
    
    return 0;
}

// FUTEX_WAKE operation
int futex_wake(uint64 uaddr, int nr_wake, uint32_t bitset) {
    if (nr_wake <= 0)
        return 0;
    
    if (uaddr % sizeof(int) != 0)
        return -22;  // -EINVAL
    
    struct proc *p = myproc();
    uint64 paddr = futex_get_physical_addr(uaddr);
    
    int woken = 0;
    
    acquire(&futex_lock);
    
    for (int i = 0; i < MAX_FUTEX_WAITERS && woken < nr_wake; i++) {
        if (!futex_waiters[i].active || futex_waiters[i].woken)
            continue;
        
        // Match by physical address or virtual address (for same process)
        int match = 0;
        if (futex_waiters[i].paddr && paddr && futex_waiters[i].paddr == paddr)
            match = 1;
        else if (futex_waiters[i].p == p && futex_waiters[i].uaddr == uaddr)
            match = 1;
        
        // Check bitset for FUTEX_WAKE_BITSET
        if (bitset != 0xffffffff && !(futex_waiters[i].bitset & bitset))
            match = 0;
        
        if (match) {
            futex_wake_waiter(i);
            woken++;
        }
    }
    
    release(&futex_lock);
    
    // Actually wake up the processes
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].active && futex_waiters[i].woken && 
            futex_waiters[i].p && futex_waiters[i].p->state == SLEEPING) {
            acquire(&futex_waiters[i].p->lock);
            if (futex_waiters[i].p->state == SLEEPING) {
                futex_waiters[i].p->state = RUNNABLE;
            }
            release(&futex_waiters[i].p->lock);
        }
    }
    
    return woken;
}

// FUTEX_REQUEUE operation
int futex_requeue(uint64 uaddr1, int nr_wake, int nr_move, uint64 uaddr2) {
    if (uaddr1 % sizeof(int) != 0 || uaddr2 % sizeof(int) != 0)
        return -22;  // -EINVAL
    
    if (uaddr1 == uaddr2)
        return -22;  // -EINVAL
    
    struct proc *p = myproc();
    uint64 paddr1 = futex_get_physical_addr(uaddr1);
    uint64 paddr2 = futex_get_physical_addr(uaddr2);
    
    int woken = 0;
    int moved = 0;
    
    acquire(&futex_lock);
    
    // First, wake up nr_wake waiters
    for (int i = 0; i < MAX_FUTEX_WAITERS && woken < nr_wake; i++) {
        if (!futex_waiters[i].active || futex_waiters[i].woken)
            continue;
        
        int match = 0;
        if (futex_waiters[i].paddr && paddr1 && futex_waiters[i].paddr == paddr1)
            match = 1;
        else if (futex_waiters[i].p == p && futex_waiters[i].uaddr == uaddr1)
            match = 1;
        
        if (match) {
            futex_wake_waiter(i);
            woken++;
        }
    }
    
    // Then, requeue nr_move waiters to uaddr2
    for (int i = 0; i < MAX_FUTEX_WAITERS && moved < nr_move; i++) {
        if (!futex_waiters[i].active || futex_waiters[i].woken)
            continue;
        
        int match = 0;
        if (futex_waiters[i].paddr && paddr1 && futex_waiters[i].paddr == paddr1)
            match = 1;
        else if (futex_waiters[i].p == p && futex_waiters[i].uaddr == uaddr1)
            match = 1;
        
        if (match) {
            futex_waiters[i].uaddr = uaddr2;
            futex_waiters[i].paddr = paddr2;
            moved++;
        }
    }
    
    release(&futex_lock);
    
    // Wake up the woken processes
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].active && futex_waiters[i].woken && 
            futex_waiters[i].p && futex_waiters[i].p->state == SLEEPING) {
            acquire(&futex_waiters[i].p->lock);
            if (futex_waiters[i].p->state == SLEEPING) {
                futex_waiters[i].p->state = RUNNABLE;
            }
            release(&futex_waiters[i].p->lock);
        }
    }
    
    return woken;
}

// FUTEX_CMP_REQUEUE operation
int futex_cmp_requeue(uint64 uaddr1, int nr_wake, int nr_move, uint64 uaddr2, int cmpval) {
    struct proc *p = myproc();
    
    // Read and compare futex value at uaddr1
    int futex_val;
    if (copyin(p->pagetable, (char*)&futex_val, uaddr1, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    if (futex_val != cmpval)
        return -1000;  // -EAGAIN (value changed)
    
    return futex_requeue(uaddr1, nr_wake, nr_move, uaddr2);
}

// FUTEX_WAKE_OP operation
int futex_wake_op(uint64 uaddr1, uint64 uaddr2, int nr_wake1, int nr_wake2, int op) {
    struct proc *p = myproc();
    
    int op_code = futex_op_op(op);
    int cmp_code = futex_op_cmp(op);
    int oparg = futex_op_oparg(op);
    int cmparg = futex_op_cmparg(op);
    
    if (op & FUTEX_OP_OPARG_SHIFT)
        oparg = 1 << oparg;
    
    // Read oldval from uaddr2
    int oldval;
    if (copyin(p->pagetable, (char*)&oldval, uaddr2, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    // Perform operation
    int newval = oldval;
    switch (op_code) {
        case FUTEX_OP_SET: newval = oparg; break;
        case FUTEX_OP_ADD: newval = oldval + oparg; break;
        case FUTEX_OP_OR:  newval = oldval | oparg; break;
        case FUTEX_OP_ANDN: newval = oldval & ~oparg; break;
        case FUTEX_OP_XOR: newval = oldval ^ oparg; break;
        default: return -22;  // -EINVAL
    }
    
    // Write newval to uaddr2
    if (copyout(p->pagetable, uaddr2, (char*)&newval, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    // Perform comparison
    int cmp_result = 0;
    switch (cmp_code) {
        case FUTEX_OP_CMP_EQ: cmp_result = (oldval == cmparg); break;
        case FUTEX_OP_CMP_NE: cmp_result = (oldval != cmparg); break;
        case FUTEX_OP_CMP_LT: cmp_result = (oldval < cmparg); break;
        case FUTEX_OP_CMP_LE: cmp_result = (oldval <= cmparg); break;
        case FUTEX_OP_CMP_GT: cmp_result = (oldval > cmparg); break;
        case FUTEX_OP_CMP_GE: cmp_result = (oldval >= cmparg); break;
        default: return -22;  // -EINVAL
    }
    
    // Wake on uaddr1
    int woken1 = futex_wake(uaddr1, nr_wake1, 0xffffffff);
    
    // Wake on uaddr2 if comparison succeeded
    int woken2 = 0;
    if (cmp_result)
        woken2 = futex_wake(uaddr2, nr_wake2, 0xffffffff);
    
    return woken1 + woken2;
}

// FUTEX_LOCK_PI - simplified implementation
int futex_lock_pi(uint64 uaddr, uint64 timeout_addr, int detect) {
    struct proc *p = myproc();
    if (!p)
        return -3;  // -ESRCH
    
    if (uaddr % sizeof(int) != 0)
        return -22;  // -EINVAL
    
    int tid = p->pid;
    int futex_val;
    
    // Try to acquire the lock (0 -> tid)
    if (copyin(p->pagetable, (char*)&futex_val, uaddr, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    if (futex_val == 0) {
        // Lock is free, try to acquire it
        if (copyout(p->pagetable, uaddr, (char*)&tid, sizeof(int)) < 0)
            return -14;  // -EFAULT
        return 0;
    }
    
    // Lock is already held, wait for it
    // Set the FUTEX_WAITERS flag if not already set
    if (!(futex_val & FUTEX_WAITERS)) {
        int new_val = futex_val | FUTEX_WAITERS;
        if (copyout(p->pagetable, uaddr, (char*)&new_val, sizeof(int)) < 0)
            return -14;  // -EFAULT
    }
    
    // Wait for the lock to be released
    return futex_wait(uaddr, futex_val | FUTEX_WAITERS, timeout_addr, 0, 0xffffffff);
}

// FUTEX_UNLOCK_PI - simplified implementation
int futex_unlock_pi(uint64 uaddr) {
    struct proc *p = myproc();
    if (!p)
        return -3;  // -ESRCH
    
    if (uaddr % sizeof(int) != 0)
        return -22;  // -EINVAL
    
    int tid = p->pid;
    int futex_val;
    
    if (copyin(p->pagetable, (char*)&futex_val, uaddr, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    // Check if we own the lock
    if ((futex_val & FUTEX_TID_MASK) != tid)
        return -1;  // -EPERM
    
    // Release the lock (set to 0)
    int new_val = 0;
    if (copyout(p->pagetable, uaddr, (char*)&new_val, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    // Wake up waiters if any
    if (futex_val & FUTEX_WAITERS) {
        futex_wake(uaddr, 1, 0xffffffff);
    }
    
    return 0;
}

// FUTEX_TRYLOCK_PI - simplified implementation
int futex_trylock_pi(uint64 uaddr) {
    struct proc *p = myproc();
    if (!p)
        return -3;  // -ESRCH
    
    if (uaddr % sizeof(int) != 0)
        return -22;  // -EINVAL
    
    int tid = p->pid;
    int futex_val;
    
    if (copyin(p->pagetable, (char*)&futex_val, uaddr, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    if (futex_val != 0)
        return -16;  // -EBUSY
    
    // Try to acquire the lock atomically
    if (copyout(p->pagetable, uaddr, (char*)&tid, sizeof(int)) < 0)
        return -14;  // -EFAULT
    
    return 0;
}

// Robust list operations
void futex_robust_list_init(struct proc *p) {
    if (!p)
        return;
    p->robust_list = NULL;
    p->robust_list_len = 0;
}

void futex_robust_list_cleanup(struct proc *p) {
    if (!p || !p->robust_list)
        return;
    
    // Walk the robust list and wake up any futexes we own
    struct robust_list_head *head = p->robust_list;
    struct robust_list *entry = head->list.next;
    
    while (entry != (struct robust_list *)head) {
        // Calculate futex address
        uint64 futex_addr = (uint64)entry + head->futex_offset;
        
        int futex_val;
        if (copyin(p->pagetable, (char*)&futex_val, futex_addr, sizeof(int)) >= 0) {
            // If we own this futex, release it and wake waiters
            if ((futex_val & FUTEX_TID_MASK) == p->pid) {
                int new_val = (futex_val & FUTEX_WAITERS) ? 0 : FUTEX_WAITERS;
                copyout(p->pagetable, futex_addr, (char*)&new_val, sizeof(int));
                if (futex_val & FUTEX_WAITERS) {
                    futex_wake(futex_addr, 1, 0xffffffff);
                }
            }
        }
        
        entry = entry->next;
    }
    
    p->robust_list = NULL;
    p->robust_list_len = 0;
}

int futex_set_robust_list(struct proc *p, struct robust_list_head *head, size_t len) {
    if (!p)
        return -3;  // -ESRCH
    
    if (len != sizeof(struct robust_list_head))
        return -22;  // -EINVAL
    
    p->robust_list = head;
    p->robust_list_len = len;
    return 0;
}

int futex_get_robust_list(struct proc *p, int pid, struct robust_list_head **head_ptr, size_t *len_ptr) {
    if (!p)
        return -3;  // -ESRCH
    
    struct proc *target = p;
    
    // If pid is 0, use current process
    // Otherwise, find the target process (simplified - no permission check for now)
    if (pid != 0 && pid != p->pid) {
        // For simplicity, only allow getting own robust list
        return -1;  // -EPERM
    }
    
    if (head_ptr)
        *head_ptr = target->robust_list;
    if (len_ptr)
        *len_ptr = target->robust_list_len;
    
    return 0;
}

// Internal PI lock/unlock for robust futex handling
int futex_lock_pi_internal(uint64 uaddr, int tid) {
    struct proc *p = myproc();
    if (!p)
        return -3;
    
    int futex_val;
    if (copyin(p->pagetable, (char*)&futex_val, uaddr, sizeof(int)) < 0)
        return -14;
    
    if (futex_val != 0)
        return -16;  // -EBUSY
    
    if (copyout(p->pagetable, uaddr, (char*)&tid, sizeof(int)) < 0)
        return -14;
    
    return 0;
}

int futex_unlock_pi_internal(uint64 uaddr, int tid) {
    struct proc *p = myproc();
    if (!p)
        return -3;
    
    int futex_val;
    if (copyin(p->pagetable, (char*)&futex_val, uaddr, sizeof(int)) < 0)
        return -14;
    
    if ((futex_val & FUTEX_TID_MASK) != tid)
        return -1;  // -EPERM
    
    int new_val = 0;
    if (copyout(p->pagetable, uaddr, (char*)&new_val, sizeof(int)) < 0)
        return -14;
    
    if (futex_val & FUTEX_WAITERS)
        futex_wake(uaddr, 1, 0xffffffff);
    
    return 0;
}
