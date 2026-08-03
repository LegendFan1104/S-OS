#include "types.h"
#include "print.h"
#include "defs.h"
#include "vmem.h"
#include "process.h"
#include "string.h"
#include "timer.h"
#include "errno-base.h"
#include "syscall_time.h"

uint64 sys_gettimeofday(uint64 tv_addr)
{
    struct proc *p = myproc();
    timeval_t tv = timer_get_time();
    return copyout(p->pagetable, tv_addr, (char *)&tv, sizeof(timeval_t));
}

static int sys_settimer_impl(int which, uint64 new_value, uint64 old_value);

int sys_getitimer(int which, uint64 old_value)
{
    return sys_settimer_impl(which, 0, old_value);
}

/**
 * @brief 获取指定时钟的时间
 *
 * @param tid       线程ID（当前实现未使用）
 * @param uaddr     用户空间存放时间结构体的地址
 * @return uint64   成功返回0，失败返回-1
 */
int sys_clock_gettime(uint64 tid, uint64 uaddr)
{
    timespec_t ts = timer_get_ntime();

    (void)tid;
    DEBUG_LOG_LEVEL(LOG_DEBUG, "clock_gettime:sec:%u,nsec:%u\n", ts.tv_sec, ts.tv_nsec);
    if (copyout(myproc()->pagetable, uaddr, (char *)&ts, sizeof(ts)) < 0)
        return -EFAULT;
    return 0;
}

int sys_clock_getres(int clk_id, uint64 uaddr)
{
    timespec_t ts = {0};

    if (clk_id < 0)
        return -EINVAL;
    ts.tv_nsec = 1000000000ULL / CLK_FREQ;
    if (ts.tv_nsec == 0)
        ts.tv_nsec = 1;
    if (uaddr && copyout(myproc()->pagetable, uaddr, (char *)&ts, sizeof(ts)) < 0)
        return -EFAULT;
    return 0;
}

/**
 * @brief 睡眠一段时间
 *        timeval_t* req   目标睡眠时间
 *        timeval_t* rem   未完成睡眠时间
 * @return int 成功返回0 失败返回-1
 */
int sleep(timespec_t *req, timespec_t *rem)
{
    proc_t *p = myproc();
    timespec_t wait = {0};
    uint64 start_ns;
    uint64 now_ns;
    uint64 deadline_ns;

    if (req == 0)
        return -EFAULT;
    if (copyin(p->pagetable, (char *)&wait, (uint64)req, sizeof(wait)) < 0)
        return -EFAULT;
    if ((int64)wait.tv_sec < 0 || wait.tv_nsec >= 1000000000ULL)
        return -EINVAL;

    start_ns = r_time() * 1000000000ULL / CLK_FREQ;
    deadline_ns = start_ns + wait.tv_sec * 1000000000ULL + wait.tv_nsec;

    acquire(&tickslock);
    while ((now_ns = r_time() * 1000000000ULL / CLK_FREQ) < deadline_ns)
    {
        if (p->killed)
        {
            release(&tickslock);
            if (rem)
            {
                timespec_t left = {0};
                uint64 remain_ns = deadline_ns - now_ns;

                left.tv_sec = remain_ns / 1000000000ULL;
                left.tv_nsec = remain_ns % 1000000000ULL;
                copyout(p->pagetable, (uint64)rem, (char *)&left, sizeof(left));
            }
            return -EINTR;
        }
        sleep_on_chan(&ticks, &tickslock);
    }
    release(&tickslock);

    if (rem)
    {
        timespec_t zero = {0};
        copyout(p->pagetable, (uint64)rem, (char *)&zero, sizeof(zero));
    }
    return 0;
}

static int sys_settimer_impl(int which, uint64 new_value, uint64 old_value)
{
    proc_t *p = myproc();
    struct itimerval current = {0};
    uint64 now = r_time();

    if (which < 0 || which > 2)
        return -EINVAL;

    current.it_interval = p->itimer.it_interval;
    if (p->timer_active && p->alarm_ticks > now)
    {
        uint64 remain = p->alarm_ticks - now;

        current.it_value.sec = remain / CLK_FREQ;
        current.it_value.usec = (remain % CLK_FREQ) * 1000000 / CLK_FREQ;
    }

    if (old_value && copyout(p->pagetable, old_value, (char *)&current, sizeof(current)) < 0)
        return -EFAULT;

    if (new_value)
    {
        struct itimerval new_timer;
        uint64 value_ticks;

        if (copyin(p->pagetable, (char *)&new_timer, new_value, sizeof(new_timer)) < 0)
            return -EFAULT;
        if (new_timer.it_value.usec >= 1000000 || new_timer.it_interval.usec >= 1000000)
            return -EINVAL;

        p->itimer = new_timer;
        value_ticks = new_timer.it_value.sec * CLK_FREQ +
                      new_timer.it_value.usec * (CLK_FREQ / 1000000);
        if (value_ticks == 0)
        {
            p->timer_active = 0;
            p->alarm_ticks = 0;
        }
        else
        {
            p->timer_active = 1;
            p->alarm_ticks = now + value_ticks;
        }
    }

    return 0;
}

int sys_settimer(int which, uint64 new_value, uint64 old_value)
{
#if DEBUG
    LOG_LEVEL(LOG_DEBUG, "[sys_settimer] which:%d, interval:%p,oldvalue:%p\n", which, new_value, old_value);
#endif
    // proc_t *p = myproc();

    // // 只支持ITIMER_REAL
    // if (which != 0)
    // { // ITIMER_REAL = 0
    //     return -1;
    // }

    // // 保存旧的定时器设置
    // if (old_value)
    // {
    //     if (copyout(p->pagetable, old_value, (char *)&p->itimer, sizeof(struct itimerval)) < 0)
    //     {
    //         return -1;
    //     }
    // }

    // // 设置新的定时器
    // if (new_value)
    // {
    //     struct itimerval new_timer;
    //     if (copyin(p->pagetable, (char *)&new_timer, new_value, sizeof(struct itimerval)) < 0)
    //     {
    //         return -1;
    //     }

    //     // 更新进程的定时器设置
    //     p->itimer = new_timer;

    //     // 计算下一次警报的tick值
    //     if (new_timer.it_value.sec || new_timer.it_value.usec)
    //     {
    //         uint64 now = r_time();
    //         uint64 interval = (uint64)new_timer.it_value.sec * CLK_FREQ +
    //                           (uint64)new_timer.it_value.usec * (CLK_FREQ / 1000000);
    //         p->alarm_ticks = now + interval;
    //         p->timer_active = 1;
    //     }
    //     else
    //     {
    //         p->timer_active = 0; // 定时器值为0则禁用
    //     }
    // }

    return sys_settimer_impl(which, new_value, old_value);
}
