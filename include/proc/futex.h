#pragma once

#include "types.h"
#include "lock/spinlock.h"

#define FUTEX_WAIT             0
#define FUTEX_WAKE             1
#define FUTEX_FD               2
#define FUTEX_REQUEUE          3
#define FUTEX_CMP_REQUEUE      4
#define FUTEX_WAKE_OP          5
#define FUTEX_LOCK_PI          6
#define FUTEX_UNLOCK_PI        7
#define FUTEX_TRYLOCK_PI       8
#define FUTEX_WAIT_BITSET      9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
#define FUTEX_LOCK_PI2        13

#define FUTEX_PRIVATE_FLAG     128
#define FUTEX_CLOCK_REALTIME   256
#define FUTEX_CMD_MASK         ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define FUTEX_OP_SET         0
#define FUTEX_OP_ADD         1
#define FUTEX_OP_OR          2
#define FUTEX_OP_ANDN        3
#define FUTEX_OP_XOR         4
#define FUTEX_OP_OPARG_SHIFT 8

#define FUTEX_OP_CMP_EQ      0
#define FUTEX_OP_CMP_NE      1
#define FUTEX_OP_CMP_LT      2
#define FUTEX_OP_CMP_LE      3
#define FUTEX_OP_CMP_GT      4
#define FUTEX_OP_CMP_GE      5

#define FUTEX_WAITERS        0x80000000
#define FUTEX_TID_MASK       0x3fffffff

// Futex waiter structure for robust futex handling
struct futex_waiter {
    int active;
    int woken;
    uint64 uaddr;           // User address (virtual)
    uint64 paddr;           // Physical address for matching
    struct proc *p;         // Waiting process
    uint32_t bitset;        // Bitset for FUTEX_WAIT_BITSET
    struct spinlock lock;   // Per-waiter lock
};

#define MAX_FUTEX_WAITERS    256

// Futex operations
void futex_init(void);
int futex_wait(uint64 uaddr, int val, uint64 timeout_addr, int clockrt, uint32_t bitset);
int futex_wake(uint64 uaddr, int nr_wake, uint32_t bitset);
int futex_requeue(uint64 uaddr1, int nr_wake, int nr_move, uint64 uaddr2);
int futex_cmp_requeue(uint64 uaddr1, int nr_wake, int nr_move, uint64 uaddr2, int cmpval);
int futex_wake_op(uint64 uaddr1, uint64 uaddr2, int nr_wake1, int nr_wake2, int op);
int futex_lock_pi(uint64 uaddr, uint64 timeout_addr, int detect);
int futex_unlock_pi(uint64 uaddr);
int futex_trylock_pi(uint64 uaddr);

// Helper functions
static inline int futex_op_op(int op) { return op & 0xf; }
static inline int futex_op_cmp(int op) { return (op >> 4) & 0xf; }
static inline int futex_op_oparg(int op) { return (op >> 8) & 0xfff; }
static inline int futex_op_cmparg(int op) { return (op >> 24) & 0xfff; }

// Robust futex support
void futex_clear_waiter(struct proc *p);
uint64 futex_get_physical_addr(uint64 uaddr);

// Robust list structures (for set_robust_list)
struct robust_list {
    struct robust_list *next;
};

struct robust_list_head {
    struct robust_list list;
    long futex_offset;
    struct robust_list *list_op_pending;
};

// Robust list operations
void futex_robust_list_init(struct proc *p);
void futex_robust_list_cleanup(struct proc *p);
int futex_set_robust_list(struct proc *p, struct robust_list_head *head, size_t len);
int futex_get_robust_list(struct proc *p, int pid, struct robust_list_head **head_ptr, size_t *len_ptr);
int futex_lock_pi_internal(uint64 uaddr, int tid);
int futex_unlock_pi_internal(uint64 uaddr, int tid);
