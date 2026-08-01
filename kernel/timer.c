#include "types.h"
#include "spinlock.h"
#include "timer.h"
#include "print.h"
#include "process.h"
#include "string.h"
//#include "syscall.h"
#include "vmem.h"
#include "cpu.h"
#include "print.h"
#include "hsai_trap.h"
#ifdef RISCV
#include "riscv.h"
#else
#include "loongarch.h"
#endif
extern proc_t pool[NPROC];

struct spinlock tickslock;
uint ticks;
uint64 timer_freq = DEFAULT_CLK_FREQ;
uint64 timer_interval = DEFAULT_CLK_FREQ / TICK_HZ;

static inline uint64 clocks_to_sec(uint64 clk)
{
    return clk / CLK_FREQ;
}

static inline uint64 clocks_to_usec_rem(uint64 clk)
{
    return (clk % CLK_FREQ) * 1000000ULL / CLK_FREQ;
}

static inline uint64 clocks_to_nsec_rem(uint64 clk)
{
    return (clk % CLK_FREQ) * 1000000000ULL / CLK_FREQ;
}

static uint64 timeval_to_clocks(const struct timeval *tv)
{
    return tv->sec * CLK_FREQ + tv->usec * (CLK_FREQ / 1000000);
}

static void refresh_process_timer(proc_t *p, uint64 now)
{
    uint64 interval;

    if (!p->timer_active || now < p->alarm_ticks)
        return;

    p->sig_pending.__val[0] |= (1UL << SIGALRM);

    interval = timeval_to_clocks(&p->itimer.it_interval);
    if (interval == 0)
    {
        p->timer_active = 0;
        p->alarm_ticks = 0;
        memset(&p->itimer.it_value, 0, sizeof(p->itimer.it_value));
        return;
    }

    do
    {
        p->alarm_ticks += interval;
    } while (p->alarm_ticks <= now);
}

#define LS7A_RTC 0x100d0100
#define LS7A_RTC_TIME_REG (*(volatile uint32_t *)((LS7A_RTC + 0x00) | dmwin_win0))

static uint64
read_rtc_seconds(void)
{
#ifdef RISCV
    /*
     * QEMU virt in the final environment exposes the timer via ACLINT/SBI,
     * but does not guarantee a Goldfish RTC MMIO device at 0x101000.
     * Touching that address faults before we can enter userspace, so for
     * RISC-V we fall back to a fixed UTC boot timestamp instead.
     */
    return 1735689600ULL;
#else
    return LS7A_RTC_TIME_REG;
#endif
}

static uint64
sanitize_boot_time(uint64 rtc_sec)
{
    if (rtc_sec < 946684800ULL || rtc_sec > 4102444800ULL)
        return 1735689600ULL;
    return rtc_sec;
}

#if !defined(RISCV)
static uint64
wait_rtc_second_change(uint64 prev, uint64 max_poll)
{
    while (max_poll-- > 0)
    {
        uint64 now = read_rtc_seconds();
        if (now != prev)
            return now;
    }
    return prev;
}
#endif

static uint64
sanitize_timer_freq(uint64 freq)
{
    if (freq < 1000000ULL || freq > 1000000000ULL)
        return DEFAULT_CLK_FREQ;
    return freq;
}

static uint64
calibrate_timer_freq_from_rtc(void)
{
#ifdef RISCV
    return DEFAULT_CLK_FREQ;
#else
    uint64 sec0, sec1;
    uint64 t0, t1;

    sec0 = read_rtc_seconds();
    sec1 = wait_rtc_second_change(sec0, 50000000ULL);
    if (sec1 == sec0)
        return 0;
    t0 = r_time();

    sec0 = sec1;
    sec1 = wait_rtc_second_change(sec0, 50000000ULL);
    if (sec1 == sec0)
        return 0;
    t1 = r_time();

    if (t1 <= t0)
        return 0;

    return t1 - t0;
#endif
}

uint64 boot_time = 0;

#if defined SBI
extern void set_timer(uint64 stime); //< 通过sbi设置下一个时钟中断
#endif

/**
 * @brief 初始化timer, RISCV
 * RISCV的计时方式是比较time计数器和stimecmp寄存器的值，相等时产生中断
 */
void 
timer_init(void) 
{
    uint64 calibrated_freq;

    initlock(&tickslock, "time");

    ticks = 0;
    boot_time = sanitize_boot_time(read_rtc_seconds());
    calibrated_freq = calibrate_timer_freq_from_rtc();
    if (calibrated_freq)
        timer_freq = sanitize_timer_freq(calibrated_freq);
    else
        timer_freq = DEFAULT_CLK_FREQ;
    timer_interval = timer_freq / TICK_HZ;
    if (timer_interval == 0)
        timer_interval = 1;
    if (FINAL_DEV_DIAG)
        printf("timer_freq = %lu Hz, timer_interval = %lu\n", timer_freq, timer_interval);
#ifdef RISCV
    #if defined SBI //< 使用sbi
    w_sie(r_sie() | SIE_STIE); //< 虽然start已经设置了SIE_STIE,这里再设置一次
	    set_timer(r_time() + INTERVAL);
    #else           //< 不使用sbi的情况
    /* enable supervisor-mode timer interrupts. */
    w_mie(r_mie() | MIE_STIE);
  
    /* enable the sstc extension (i.e. stimecmp). */
    w_menvcfg(r_menvcfg() | (1L << 63)); 
  
    /* allow supervisor to use stimecmp and time. */
    w_mcounteren(r_mcounteren() | 2);
  
    /* ask for the very first timer interrupt. */
    w_stimecmp(r_time() + INTERVAL);
    #endif
#else
    countdown_timer_init();
#endif
}

#ifdef RISCV
/**
 * @brief 设置下一次定时器中断产生, RISCV
 * 
 */
void 
set_next_timeout(void) 
{
    uint64 next = r_time() + INTERVAL;
    /* TODO 这里未来可能会改成SBI形式 */
    #if defined SBI
    set_timer(next);
    #else
    w_stimecmp(next);
    #endif
}
#else   ///< loongarch
/**
 * @brief 设置计时器产生中断的时间, 自动装载, loongarch
 * loongarch的计时器是使用倒计时，到0时产生中断，可以设置是否自动装载
 */
void
countdown_timer_init(void)
{
    uint64 prcfg1_val;
    /* 读取特权资源配置信息1 */
    prcfg1_val = r_csr_prcfg1();

    /* 读取定时器(Timer)的有效位数 */
    uint64 timerbits = FIELD_GET(prcfg1_val,PRCFG1_TIMERBITS_LEN,
                                PRCFG1_TIMERBITS_SHIFT) + 1;
    
    /* 判断有效位是否合法 */
    assert (timerbits >= 0 && timerbits <=64, 
        "countdown_timer_init: timerbits is invalid");
    
    /* 设置定时器配置寄存器，自减至0时，自动装载initval的值 */
    uint64 tcfg_val = FIELD_WRITE(1,TCFG_EN_LEN,TCFG_EN_SHIFT) | 
          FIELD_WRITE(1,TCFG_RERIODIC_LEN,TCFG_RERIODIC_SHIFT) | 
          FIELD_WRITE(INTERVAL,TCFG_INITVAL_LEN(timerbits),TCFG_INITVAL_SHIFT);
    
    /* 启动countdown_timer */
    w_csr_tcfg(tcfg_val);
}
#endif

/**
 * @brief 自加ticks
 * 
 */
void 
timer_tick(void) 
{
#if DEBUG
    printf("timer tick\n");
#endif
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
    uint64 now = r_time();
    for (proc_t *p = pool; p < pool + NPROC; p++)
    {
        acquire(&p->lock);
        if (p->state != UNUSED)
            refresh_process_timer(p, now);
        release(&p->lock);
    }
#ifdef RISCV
    set_next_timeout();
#endif
}

/**
 * @brief 得到当前进程的运行时间
 * 
 * @return uint64 成功返回0
 */
uint64 
get_times(uint64 utms) 
{
    struct tms ptms;
    ptms.tms_utime = myproc()->utime;
    ptms.tms_stime = myproc()->ktime;
    ptms.tms_cstime = 1;
    ptms.tms_cutime = 1;

    /* 加上所有孩子进程的时间 */
    struct proc *p;
    for (p = pool; p < pool + NPROC; p++) 
    {
        acquire(&p->lock);
        if (p->parent == myproc()) {
            ptms.tms_cutime += p->utime;
            ptms.tms_cstime += p->ktime;
        }
        release(&p->lock);
    }
    copyout(myproc()->pagetable, utms, (char *)&ptms, sizeof(ptms));
    return 0;
}



timeval_t timer_get_time(){
    timeval_t tv;
    uint64 clk = r_time();

    tv.sec = boot_time + clocks_to_sec(clk);
    tv.usec = clocks_to_usec_rem(clk);
    return tv;
}
timespec_t timer_get_ntime() {
    timespec_t ts;
    uint64 clk = r_time();

    ts.tv_sec = boot_time + clocks_to_sec(clk);
    ts.tv_nsec = clocks_to_nsec_rem(clk);
    
    return ts;
}
