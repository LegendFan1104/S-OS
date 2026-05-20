
#include "lock/semaphore.h"
#include "defs.h"

void sem_init(sem *s, int value, char *name) {
    s->value = value;
    s->top = 0;
    s->wakeup = 0;
    initlock(&s->lock, name);
    for (int i = 0; i < NPROC; i++) {
        s->wait_list[i] = 0;
    }
}
/*
 *
 *这里的PV操作比较暴力，将wait_list全部唤醒，待优化
 *TODO：
 *应该用信号来进行唤醒
 */
void sem_p(sem *s) {
    acquire(&s->lock);
    if (s->value == 0) {
        do {
            struct proc *p = myproc();
            acquire(&p->lock);
            s->wait_list[s->top++] = myproc();
            s->wait_list[s->top - 1]->state = SLEEPING;
            release(&s->lock);
            sched();
            release(&p->lock);
            if (killed(p)) {
                exit(-1);
            }
            acquire(&s->lock);
        } while (s->wakeup == 0);
        s->wakeup --;
    }
    release(&s->lock);
}

void sem_v(sem *s) {
    acquire(&s->lock);
    s->value++;
    if (s->value <= 0) {
        s->wakeup ++;
        for (int i=0;i<s->top;i++) {
            s->wait_list[i]->state = RUNNABLE;
        }
        s->top = 0;
    }
    release(&s->lock);
}




