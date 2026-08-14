// 处理两个架构的异常。为kernel提供架构无关的接口
//
//
#include "types.h"
#include "trap.h"
#include "print.h"
#include "virt.h"
#include "plic.h"
#include "process.h"
#include "cpu.h"
#include "timer.h"
#include "vmem.h"
#include "vma.h"
#include "vfs_ext4.h"
#include "ext4_oflags.h"
#if defined RISCV
#include <riscv.h>
#include "riscv_memlayout.h"
#else
#include "loongarch.h"
#endif
#include "test.h"
#include "string.h"
#include "futex.h"

/* 两个架构的trampoline函数名称一致 */
extern char uservec[];    ///< trampoline 用户态异常，陷入。hsai_set_usertrap使用
#if defined(BOARD_LS2K)
/* 2K1000 cannot reliably fetch the trampoline through its high virtual
 * mapping.  The linker symbol is the DMW alias used by the board path. */
extern void userret(uint64 trapframe_addr, uint64 pgdl);
#else
extern char userret[];    ///< trampoline 进入用户态。hsai_usertrapret使用
#endif
extern void kernelvec();  ///< 外部中断/异常入口
extern char trampoline[]; ///< trampoline 代码段的起始地址
extern void handle_tlbr();
extern void handle_merr();

#if !defined RISCV
static void diag_walk_chain(pgtbl_t pt, uint64 va);
#endif

int devintr(void); ///< 中断判断函数

/* usertrap()需要这两个 */
#define SSTATUS_SPP (1L << 8)                     ///< Previous mode, 1=Supervisor, 0=User
extern void syscall(struct trapframe *trapframe); ///< 系统调用中断处理函数

/* hsai_set_trapframe_kernel_sp需要这个 */
extern struct proc *myproc();

/* 把idle和p交换.再swtch.S中，目前没有使用 */
extern void swtch(struct context *idle, struct context *p);

/**
 * @brief 对loongarch设置ecfg,应该只在初始化时设置一次。RISCV设置中断入口。开启外部中断和时钟中断
 *
 * Riscv对应的是设置sie，但是已经在start.c中设置了，同时设置中断入口。而loongarch没有M态的初始化。
 * 这里只对loongarch执行操作，riscv什么都不做
 */
void hsai_trap_init(void)
{
#if defined RISCV
    /*
     * OpenSBI enters the kernel in S-mode, so external/software interrupt
     * bits must be enabled here. timer_init() only turns on STIE; without
     * SEIE, virtio disk interrupts never arrive and the first fs_mount()
     * in forkret() stalls forever.
     */
    w_sie(r_sie() | SIE_SEIE | SIE_SSIE);
    w_stvec((uint64)kernelvec); ///< 设置内核trap入口
#else
    uint32 ecfg = (0U << CSR_ECFG_VS_SHIFT) | HWI_VEC | TI_VEC; ///< 例外配置
    w_csr_ecfg(ecfg);                                           ///< 设置例外配置
    w_csr_eentry((uint64)kernelvec);                            ///< 设置内核trap入口
    /* These exception-vector CSRs receive physical addresses.  The 2K1000
     * does not decode a 0x9000 DMW tag here, unlike the permissive QEMU. */
    w_csr_tlbrentry((uint64)handle_tlbr & ~dmwin_mask);
    w_csr_merrentry((uint64)handle_merr & ~dmwin_mask);
    {
        uint64 rt, rt2;
        __asm__ volatile("csrrd %0, 0x88" : "=r"(rt));
        __asm__ volatile("csrrd %0, 0x93" : "=r"(rt2));
        if (FINAL_DEV_DIAG)
            printf("[diag][trap-init] kernelvec=%p eentry=%p "
                   "tlbr=%p tlbr_csr=%p merr=%p merr_csr=%p\n",
                   (void *)kernelvec, (void *)r_csr_eentry(),
                   (void *)handle_tlbr, (void *)rt,
                   (void *)handle_merr, (void *)rt2);
    }
    timer_init();                                               ///< 启动时钟中断

    /* LoongArch glibc (the gcc-13.2 sysroot copy shipped in /glibc/lib)
     * uses FPU and LSX in hot paths such as memcpy; enable all SIMD
     * extensions or those functions trap with FPE. */
    w_csr_euen(FPE_ENABLE | EUEN_LSXEN | EUEN_LASXEN);
#endif
}

void machine_trap(void)
{
    panic("machine error");
}

static int pagefault_handler_locked(uint64 addr)
{
    proc_t *p = myproc();
    if (p == NULL || addr >= MAXVA)
        return -1;
    struct vma *find_vma = find_mmap_vma(p->vma);
    int flag = 0;
    int perm = 0;
    int npages = 1;

    // +++ 关键修复：确保地址页面对齐 +++
    uint64 aligned_addr = PGROUNDDOWN(addr);

    /* The page is already mapped.  LoongArch's software TLB walk raises PME
     * when a store hits a writable page whose dirty bit is clear; mark the
     * leaf dirty and retry.  A store to a present read-only leaf is a real
     * protection violation. */
    pte_t *cur_pte = walk(p->pagetable, aligned_addr, 0);
    if (cur_pte != NULL && (*cur_pte & PTE_V))
    {
        if ((*cur_pte & PTE_W) == 0)
        {
            printf("[diag][pf-ro] pid=%d addr=%p pte=%p\n",
                   p->pid, addr, *cur_pte);
            return -1;
        }
        *cur_pte |= PTE_D;
        return 0;
    }

    if (addr <= p->sz)
    {
        flag = 1;
        perm = PTE_R | PTE_W | PTE_X | PTE_D | PTE_U;
        // npages = (addr + (16)*PGSIZE >PGROUNDUP(p->sz) )? (PGROUNDUP(p->sz) - PGROUNDDOWN(addr)) / PGSIZE : 16;
    }
    else
    {
        while (find_vma != p->vma)
        {
            if (addr >= find_vma->end)
                find_vma = find_vma->next;
            else if (addr >= find_vma->addr && addr <= find_vma->end)
            {
                flag = 1;
                perm = find_vma->perm | PTE_U;
                // npages = (addr + 16 * PGSIZE > PGROUNDUP(find_vma->end) )?  (PGROUNDUP(find_vma->end) -  PGROUNDDOWN(addr)) / PGSIZE : (16);
                break;
            }
            else
            {
#if defined RISCV
                printf("[diag][pf-gap] pid=%d addr=%p epc=%p sz=%p\n",
                       p->pid, addr, p->trapframe ? p->trapframe->epc : 0, p->sz);
#else
                printf("[diag][pf-gap] pid=%d addr=%p era=%p badi=%p sz=%p\n",
                       p->pid, addr, p->trapframe ? p->trapframe->era : 0,
                       r_csr_badi(), p->sz);
                if (p->trapframe)
                    printf("  tf: a0=%p sp=%p tp=%p ra=%p fp=%p s4=%p s5=%p s6=%p s7=%p t1=%p s8=%p\n",
                           p->trapframe->a0, p->trapframe->sp,
                           p->trapframe->tp, p->trapframe->ra,
                           p->trapframe->fp,
                           p->trapframe->s4, p->trapframe->s5,
                           p->trapframe->s6, p->trapframe->s7,
                           p->trapframe->t1, p->trapframe->s8);
                if (p->trapframe && p->trapframe->s7)
                {
                    uint64 s7_page = PGROUNDDOWN(p->trapframe->s7);
                    pte_t *s7p = walk(p->pagetable, s7_page, 0);
                    uint64 s7_pa = s7p && (*s7p & PTE_V) ? PTE2PA(*s7p) : 0;
                    printf("  s7 page=%p pte=%p pa=%p\n", s7_page,
                           s7p ? *s7p : 0, s7_pa);
                    if (s7_pa)
                    {
                        uint64 *m = (uint64 *)(s7_pa | dmwin_win0);
                        uint64 off = p->trapframe->s7 - s7_page;
                        printf("  s7 words: %p %p %p\n",
                               m[off / 8], m[off / 8 + 1], m[off / 8 + 2]);
                        printf("  s7 dump:");
                        for (int i = 0; i < 64; i++)
                        {
                            if (i % 4 == 0)
                                printf("\n  %p:", s7_page + (off & ~0x1fULL) + i * 8);
                            printf(" %p", m[(off & ~0x1fULL) / 8 + i]);
                        }
                        printf("\n");
                    }
                }
                struct vma *dv = p->vma;
                if (dv)
                {
                    struct vma *it = dv->next;
                    int n = 0;
                    while (it != dv && n < 20)
                    {
                        printf("  vma[%d] %p-%p type=%d perm=0x%lx\n",
                               n, it->addr, it->end, it->type, it->perm);
                        it = it->next;
                        n++;
                    }
                }
#endif
                return -1;
            }
        }
    }
    // 找到缺页对应的vma
    if (!flag)
    {
        return -1;
    }
    // DEBUG_LOG_LEVEL(DEBUG, "pagefault addr:%p,p->sz:%p,alloc page num:%d\n", addr, p->sz, npages);

    char *pa;
    pa = pmem_alloc_pages(npages);

    // +++ 关键修复：验证分配的内存页面对齐 +++
    if (pa == NULL)
    {
        panic("pmem_alloc_pages failed for %d pages\n", npages);
        return -1;
    }

    if ((uint64)pa % PGSIZE != 0)
    {
        printf("WARNING: pmem_alloc_pages returned unaligned address %p\n", pa);
        pmem_free_pages(pa, npages);
        return -1;
    }

    // 确保分配的内存完全清零
    memset(pa, 0, npages * PGSIZE);
    pte_t *pte = walk(p->pagetable, aligned_addr, 0);
    if (pte && (*pte & PTE_V))
    {
        DEBUG_LOG_LEVEL(LOG_WARNING, "address:aligned_addr:%p is already mapped!\n", aligned_addr);
        *pte |= perm;
    }
    else
    {
    if (mappages(p->pagetable, aligned_addr, (uint64)pa, npages * PGSIZE, perm) < 0)
    {
        panic("mappages failed\n");
        pmem_free_pages(pa, npages);
        return -1;
    }
    }

    return 0;
}

int pagefault_handler(uint64 addr)
{
    proc_t *p = myproc();
    int ret;

    if (p == NULL || addr >= MAXVA)
        return -1;
    acquire(&p->vma_lock);
    ret = pagefault_handler_locked(addr);
    release(&p->vma_lock);
    return ret;
}

/**
 * @brief 设置异常处理函数到uservec,对于U态的异常
 */
void hsai_set_usertrap(void)
{
#if defined RISCV // trap_init
    w_stvec(TRAMPOLINE + (uservec - trampoline));
#else
    w_csr_eentry((uint64)uservec & ~0x3); //
#endif
}

/**
 * @brief 设置好sstatus或prmd,准备进入U态
 */
void hsai_set_csr_to_usermode(void) // 设置好csr寄存器，准备进入U态
{
#if defined RISCV
    // set S Previous Privilege mode to User.
    uint64 x = r_sstatus();
    x &= ~SSTATUS_SPP; ///< clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; ///< enable interrupts in user mode
    // x |= SSTATUS_SIE; 	///< 委托给S态处理中断，要使能
    w_sstatus(x);
#else
    // 类似设置sstatus
    uint32 x = r_csr_prmd();
    x |= PRMD_PPLV; ///< set PPLV to 3 for user mode
    x |= PRMD_PIE;  ///< enable interrupts in user mode
    w_csr_prmd(x);
#endif
}

/**
 * @brief 设置sepc或era,返回用户态时跳转到用户程序
 */
void hsai_set_csr_sepc(uint64 addr) ///< 设置sepc, sret时跳转
{
#if defined RISCV
    w_sepc(addr);
#else
    /* 设置era,指令ertn使用。S态进入U态 */
    w_csr_era((uint64)(void *)addr);
#endif
}

/**
 * @brief 得到对应trapframe的参数
 *
 * @param trapframe
 * @param register_num
 * @return uint64
 */
uint64
hsai_get_arg(struct trapframe *trapframe, uint64 register_num) ///< 从trapframe获取参数a0-a7
{
    switch (register_num) ///< 从0开始编号，0返回a0
    {
    case 0:
        return trapframe->a0;
        break;
    case 1:
        return trapframe->a1;
        break;
    case 2:
        return trapframe->a2;
        break;
    case 3:
        return trapframe->a3;
        break;
    case 4:
        return trapframe->a4;
        break;
    case 5:
        return trapframe->a5;
        break;
    case 6:
        return trapframe->a6;
        break;
    case 7:
        return trapframe->a7;
        break;
    default:
        printf("无效的regisrer_num: %d", register_num);
        return -1;
        break;
    }
    return 0;
}

/**
 * @brief 交换old和new线程上下文
 *
 * @param old 旧线程
 * @param new 新线程
 */
void hsai_swtch(struct context *old, struct context *new)
{
#if defined RISCV
    swtch(old, new);
#else
    swtch(old, new);
#endif
}

/**
 * @brief 设置线程内核栈
 *
 * @param trapframe
 * @param value
 */
void hsai_set_trapframe_kernel_sp(struct trapframe *trapframe, uint64 value) // 修改线程内核栈
{
#if defined RISCV
    trapframe->kernel_sp = value;
#else
    trapframe->kernel_sp = value;
#endif
}

/**
 * @brief 设置内核的用户态陷入处理函数
 *
 * @param trapframe
 */
// 为给定的trapframe设置usertrap,在trampoline保存状态后usertrap处理陷入或异常
// 这个usertrap地址是固定的
void hsai_set_trapframe_kernel_trap(struct trapframe *trapframe)
{
#if defined RISCV
    trapframe->kernel_trap = (uint64)usertrap;
#else
    trapframe->kernel_trap = (uint64)usertrap;
#endif
}

/**
 * @brief 设置trapframe的返回地址
 *
 * @param trapframe
 * @param value
 */
void hsai_set_trapframe_epc(struct trapframe *trapframe, uint64 value) // 修改返回地址，loongarch的Trapframe为era,意义相同
{
#if defined RISCV
    trapframe->epc = value;
#else
    trapframe->era = value;
#endif
}

/**
 * @brief 设置trapframe的用户栈
 *
 * @param trapframe
 * @param value
 */
void hsai_set_trapframe_user_sp(struct trapframe *trapframe, uint64 value) // 修改用户态的栈
{
#if defined RISCV
    trapframe->sp = value;
#else
    trapframe->sp = value;
#endif
}

/**
 * @brief 设置trapframe的内核页表
 *
 * @param trapframe
 */
void hsai_set_trapframe_pagetable(struct trapframe *trapframe) // 修改页表
{
#if defined RISCV
    trapframe->kernel_satp = r_satp();
#else
    trapframe->kernel_pgdl = r_csr_pgdl();
#endif
}

static void
hsai_set_trapframe_kernel_hartid(struct trapframe *trapframe)
{
#if defined RISCV
    trapframe->kernel_hartid = r_tp();
#else
    trapframe->kernel_hartid = r_tp();
#endif
}

/**
 * @brief 开启时钟中断使能
 */
void hsai_clock_intr_on()
{
#if RISCV
    w_sie(r_sie() | SIE_STIE);
#else
    w_csr_ecfg(r_csr_ecfg() | TI_VEC);
#endif
}

/**
 * @brief 关闭时钟中断使能
 */
void hsai_clock_intr_off()
{
#if RISCV
    w_sie(r_sie() & ~SIE_STIE);
#else
    w_csr_ecfg(r_csr_ecfg() & ~TI_VEC);
#endif
}

// extern void userret(uint64 trapframe_addr, uint64 pgdl);
// 如果是第一次进入用户程序，调用usertrapret之前，还要初始化trapframe->sp
void hsai_usertrapret()
{
    proc_t *p = myproc();
    struct trapframe *trapframe = p->trapframe;
#if defined RISCV
    static int diag_userret_once = 0;
#endif
    intr_off();
    hsai_set_usertrap();

    /* 使用当前线程的内核栈而不是进程的主栈 */
    if (p->main_thread->kstack != p->kstack)
        hsai_set_trapframe_kernel_sp(trapframe, p->main_thread->kstack + KSTACKSIZE);
    else
        hsai_set_trapframe_kernel_sp(trapframe, p->kstack + KSTACKSIZE);

    hsai_set_trapframe_pagetable(trapframe);
    hsai_set_trapframe_kernel_hartid(trapframe);
    hsai_set_trapframe_kernel_trap(trapframe);
    hsai_set_csr_to_usermode();
#if defined RISCV ///< 后续系统调用，只需要下面的代码
    //intr_off();
    hsai_set_csr_sepc(trapframe->epc);

    uint64 satp = MAKE_SATP(myproc()->pagetable);
    uint64 fn = TRAMPOLINE + (userret - trampoline);
    if (FINAL_DEV_DIAG && diag_userret_once == 0)
    {
        diag_userret_once = 1;
        printf("[diag][usertrapret] pid=%d epc=%p sp=%p satp=%p fn=%p trapframe=%p\n",
               p->pid, trapframe->epc, trapframe->sp, satp, fn, trapframe);
    }
#if DEBUG
    // printf("epc: 0x%p  ", trapframe->epc);
    // printf("即将跳转: %p\n", fn);
#endif

    ((void (*)(uint64, uint64))fn)(TRAPFRAME, satp);

#else ///< loongarch
    //intr_off();
    // 设置ertn的返回地址
    hsai_set_csr_sepc(trapframe->era);
#if !defined(BOARD_LS2K)
    uint64 fn = TRAMPOLINE + (userret - trampoline);
#endif
#if DEBUG
    // printf("epc: 0x%p  ", trapframe->era);
    // printf("即将跳转: %p\n", fn);
#endif
    /* 硬件 TLB 重填从 PGDL 取页表根：必须是未带 0x9000 DMW 标记的
     * 物理地址（真板内存控制器不解码 0x9000 前缀，QEMU 不检查）。 */
    volatile uint64 pgdl =
        (uint64)(myproc()->pagetable) & ~dmwin_mask;
    {
        static int diag_la_userret_once = 0;
        if (FINAL_DEV_DIAG && diag_la_userret_once == 0)
        {
            diag_la_userret_once = 1;
#if defined(BOARD_LS2K)
            printf("[diag][usertrapret] pid=%d era=%p sp=%p pgdl=%p "
                   "direct_fn=%p trapframe=%p\n",
                   myproc()->pid, trapframe->era, trapframe->sp,
                   (void *)pgdl, (void *)(uint64)userret,
                   (void *)trapframe);
#else
            printf("[diag][usertrapret] pid=%d era=%p sp=%p pgdl=%p fn=%p\n",
                   myproc()->pid, trapframe->era, trapframe->sp,
                   (void *)pgdl, (void *)fn);
#endif
        }
    }
    /* 实验：进用户态前 dump 用户页表关键 VA 的叶子 PTE，
     * 确认映射与 PFN/标志位是否就绪（真板硬件遍历依赖这些）。 */
    {
        static int diag_la_pte_once = 0;
        if (FINAL_DEV_DIAG && diag_la_pte_once == 0)
        {
            diag_la_pte_once = 1;
            uint64 stack_top_page = USER_STACK_TOP - PGSIZE;
            uint64 vas[7] = { 0, 0x1000, stack_top_page - PGSIZE, stack_top_page,
                              0xffffff000, 0x3fffffffe000, 0x3ffffffff000 };
            int i;
            for (i = 0; i < 7; i++)
            {
                pte_t *pte = walk(p->pagetable, vas[i], 0);
                printf("[diag][pte] va=0x%llx pte=0x%llx\n",
                       (unsigned long long)vas[i],
                       pte ? (unsigned long long)*pte : 0ULL);
            }
            diag_walk_chain(p->pagetable, stack_top_page); /* 用户栈顶页 */
            diag_walk_chain(p->pagetable, 0);            /* 参考：代码页 */
        }
    }
#if defined(BOARD_LS2K)
    /* Keep trampoline and trapframe on the DMW alias until ertn.  This is
     * the proven 2K1000 board path; only the final user instruction fetch
     * needs the user page table. */
    userret((uint64)trapframe, pgdl);
#else
    ((void (*)(uint64, uint64))fn)(TRAPFRAME, pgdl); // 可以传参
#endif
#endif
}

extern void list_file(const char *path);
void forkret(void)
{
    static int first = 1;
#if !defined RISCV
    static int diag_la_forkret_once = 0;
    if (FINAL_DEV_DIAG && diag_la_forkret_once == 0)
    {
        diag_la_forkret_once = 1;
        printf("[diag][forkret-enter] pid=%d first=%d\n",
               myproc()->pid, first);
    }
#endif
#if defined RISCV
    static int diag_forkret_once = 0;
    if (FINAL_DEV_DIAG && diag_forkret_once == 0)
    {
        diag_forkret_once = 1;
        printf("[diag][forkret-enter] pid=%d epc=%p sp=%p first=%d\n",
               myproc()->pid, myproc()->trapframe->epc, myproc()->trapframe->sp, first);
    }
#endif
    release(&myproc()->lock);
    if (first)
    {
        // File system initialization must be run in the context of a
        // regular process (e.g., because it calls sleep), and thus cannot
        // be run from main().
        first = 0;
        if (FINAL_DEV_DIAG)
            printf("[diag][forkret] calling fs_mount\n");
        fs_mount(ROOTDEV, EXT4, "/", 0, NULL); // 挂载文件系统
        if (FINAL_DEV_DIAG)
            printf("[diag][forkret] fs_mount done\n");
        dir_init();
        futex_init();

        /* init线程cwd设置 */
        struct file_vnode *cwd = &(myproc()->cwd);
        strcpy(cwd->path, "/");
        cwd->fs = get_fs_by_type(EXT4);

        /* 列目录 */
        // #if DEBUG
        //         list_file("/");
        //         list_file("/musl");
        // #endif
        /*
         * NOTE: DEBUG用
         * forkret好像是内核态的，我在forkret中测试，所以
         * 用了isnotforkret，后面可能要删掉
         */
        extern bool isnotforkret;
        isnotforkret = true;
#if defined RISCV
        if (FINAL_DEV_DIAG)
            printf("[diag][forkret-initfs] cwd=%s fs=%p\n", myproc()->cwd.path, myproc()->cwd.fs);
#endif
    }
    hsai_usertrapret();
}
///< 如果已经进入了U态，每次系统调用完成后返回时只需要如下就可以（不考虑虚拟内存
// 如果是第一次进入用户程序，调用usertrapret之前，还要初始化trapframe->sp
//  void minium_usertrap(struct trapframe *trapframe)
//  {
//  	#if defined RISCV
//  		hsai_set_csr_sepc(trapframe->epc);
//  		userret((uint64)trapframe);
//      #else
//  		hsai_set_csr_sepc(trapframe->era);
//  		userret((uint64)trapframe);
//      #endif
//  }

/**
 * @brief 用户态中断和异常处理函数
 *
 */
#if !defined RISCV
/* TLB 重填失败诊断：由 tlbrefill.S 在遍历失败时跳到此处（已切到
 * .bss 里的安全栈），打印 PGD/BADV/ERA/失败级数后死循环。 */
extern uint64 tlbr_diag_stage, tlbr_diag_pgd, tlbr_diag_badv;
extern uint64 tlbr_diag_era, tlbr_diag_val;
void tlbr_diag(void)
{
    printf("[tlbr-DIAG] stage=%llu pgd=0x%llx badv=0x%llx era=0x%llx "
           "val=0x%llx\n",
           (unsigned long long)tlbr_diag_stage,
           (unsigned long long)tlbr_diag_pgd,
           (unsigned long long)tlbr_diag_badv,
           (unsigned long long)tlbr_diag_era,
           (unsigned long long)tlbr_diag_val);
    for (;;)
        ;
}
#endif

/* 调试：打印某个 VA 的完整页表遍历链（l3/l2/l1/leaf），定位断在哪一级 */
#if !defined RISCV
static void diag_walk_chain(pgtbl_t pt, uint64 va)
{
    pte_t *pte = &pt[PX(3, va)];
    printf("[diag][chain] va=0x%llx l3=0x%llx", (unsigned long long)va,
           (unsigned long long)*pte);
    if (!(*pte & PTE_V))
    {
        printf("\n");
        return;
    }
    pt = (pgtbl_t)((PTE2PA(*pte)) | dmwin_win0);
    pte = &pt[PX(2, va)];
    printf(" l2=0x%llx", (unsigned long long)*pte);
    if (!(*pte & PTE_V))
    {
        printf("\n");
        return;
    }
    pt = (pgtbl_t)((PTE2PA(*pte)) | dmwin_win0);
    pte = &pt[PX(1, va)];
    printf(" l1=0x%llx", (unsigned long long)*pte);
    if (!(*pte & PTE_V))
    {
        printf("\n");
        return;
    }
    pt = (pgtbl_t)((PTE2PA(*pte)) | dmwin_win0);
    pte = &pt[PX(0, va)];
    printf(" leaf=0x%llx\n", (unsigned long long)*pte);
}
#endif

// 其实xv6-loongarch从uservec进入usertrap时，a0也是trapframe.只不过xv6-loongarch声明为usertrap(void)。我们是可以用a0当trapframe的
void usertrap(void)
{
    struct proc *p = myproc();
    struct trapframe *trapframe = p->trapframe;
    int which_dev = 0;
#if defined RISCV
    static int diag_usertrap_count = 0;

    w_stvec((uint64)kernelvec);

    trapframe->epc = r_sepc();
    if (FINAL_DEV_DIAG && diag_usertrap_count < 8)
    {
        diag_usertrap_count++;
        printf("[diag][usertrap] pid=%d cause=%p epc=%p stval=%p\n",
               p ? p->pid : -1, r_scause(), trapframe->epc, r_stval());
    }
    if ((r_sstatus() & SSTATUS_SPP) != 0)
    {
        panic("usertrap: not from user mode");
        while (1)
            ;
    }

    uint64 cause = r_scause();
    if ((which_dev = devintr()) != 0)
    {
        if (which_dev == 2 && p != 0)
        { /* 时钟中断 */
            // proc_t *p = myproc();
            // if (p && p->timer_active && r_time() >= p->alarm_ticks)
            // {
            //     // 发送SIGALRM信号
            //     kill(p->pid, SIGALRM);

            //     // 如果是周期性定时器，重新设置
            //     if (p->itimer.it_interval.sec || p->itimer.it_interval.usec)
            //     {
            //         uint64 interval = (uint64)p->itimer.it_interval.sec * CLK_FREQ +
            //                           (uint64)p->itimer.it_interval.usec * (CLK_FREQ / 1000000);
            //         p->alarm_ticks = r_time() + interval;
            //     }
            //     else
            //     {
            //         p->timer_active = 0;
            //     }
            // }
            p->utime++;
            yield();
            hsai_usertrapret();
        }
        // TODO 其他中断
    }
    else
    { /* 异常 */
        if (cause == UserEnvCall)
        {
            if (p->killed)
            {
                p->term_signal = p->killed;
                exit(0);
            }
            // printf(BLUE_COLOR_PRINT"epc: %x",trapframe->epc);
            trapframe->epc += 4;
            intr_on();
            syscall(trapframe);
            hsai_usertrapret();
        }
        switch (cause)
        {
        case StoreMisaligned:
        case LoadMisaligned:
        case InstructionMisaligned:
        case InstructionPageFault:
            printf("%d in application, bad addr = %p, bad instruction = %p, core "
                   "dumped.\n",
                   cause, r_stval(), trapframe->epc);
            printf("a0=%p\na1=%p\na2=%p\na3=%p\na4=%p\na5=%p\na6=%p\na7=%p\nsp=%p\n", trapframe->a0, trapframe->a1, trapframe->a2, trapframe->a3, trapframe->a4, trapframe->a5, trapframe->a6, trapframe->a7, trapframe->sp);
            printf("p->pid=%d, p->sz=%d\n", p->pid, p->sz);

            // For instruction page fault, check the page table entry at the faulting instruction address
            uint64 fault_addr = (cause == InstructionPageFault) ? trapframe->epc : r_stval();
            pte_t *pte = walk(p->pagetable, fault_addr, 0);
            if (pte != NULL && (*pte & PTE_V))
            {
                printf("PTE for addr 0x%p: valid=%d, read=%d, write=%d, exec=%d, user=%d, full_pte=0x%p\n",
                       fault_addr,
                       !!(*pte & PTE_V),
                       !!(*pte & PTE_R),
                       !!(*pte & PTE_W),
                       !!(*pte & PTE_X),
                       !!(*pte & PTE_U),
                       *pte);
            }
            else
            {
                printf("PTE for addr 0x%p: not found or invalid (pte=%p)\n", fault_addr, pte);
            }

            // Also check the stval address if different from epc
            if (cause == InstructionPageFault && r_stval() != trapframe->epc)
            {
                pte_t *stval_pte = walk(p->pagetable, r_stval(), 0);
                if (stval_pte != NULL && (*stval_pte & PTE_V))
                {
                    printf("STVAL PTE for addr 0x%p: valid=%d, read=%d, write=%d, exec=%d, user=%d, full_pte=0x%p\n",
                           r_stval(),
                           !!(*stval_pte & PTE_V),
                           !!(*stval_pte & PTE_R),
                           !!(*stval_pte & PTE_W),
                           !!(*stval_pte & PTE_X),
                           !!(*stval_pte & PTE_U),
                           *stval_pte);
                }
                else
                {
                    printf("STVAL PTE for addr 0x%p: not found or invalid (pte=%p)\n", r_stval(), stval_pte);
                }
            }
            p->term_signal = (cause == InstructionPageFault) ? SIGSEGV :
                             (cause == InstructionMisaligned) ? SIGILL : SIGBUS;
            exit(0);
        case IllegalInstruction:
            printf("IllegalInstruction in application, epc = %p, core dumped.",
                   trapframe->epc);
            p->term_signal = SIGILL;
            exit(0);
        case LoadPageFault:
        case StorePageFault:
            pagefault_handler(r_stval());
            hsai_usertrapret();
            break;
        default:
            printf("unknown trap: %p, stval = %p sepc = %p\n", r_scause(),
                   r_stval(), r_sepc());
            p->term_signal = SIGSEGV;
            exit(0);
        }
    }
#else
    /*
     * 我真的服了，xv6-loongarch的trampoline不写入era，要在usertrap保存。
     * riscv都是在trampoline保存的。就这样，在usertrap保存era,不在trampoline保存了
     */
    w_csr_eentry((uint64)kernelvec);

    // #if DEBUG
    //     printf("usertrap():handling exception\n");
    //     uint32 info = r_csr_crmd();
    //     printf("usertrap(): crmd=0x%x\n", info);
    //     info = r_csr_prmd();
    //     printf("usertrap(): prmd=0x%x\n", info);
    //     info = r_csr_estat();
    //     printf("usertrap(): estat=0x%x\n", info);
    //     info = r_csr_era();
    //     printf("usertrap(): era=0x%x\n", info);
    //     info = r_csr_ecfg();
    //     printf("usertrap(): ecfg=0x%x\n", info);
    //     info = r_csr_badi();
    //     printf("usertrap(): badi=0x%x\n", info);
    //     info = r_csr_badv();
    //     printf("usertrap(): badv=0x%x\n\n", info);
    // #endif
    trapframe->era = r_csr_era(); ///< 记录trap发生地址
    {
        static int diag_la_usertrap_count = 0;
        if (FINAL_DEV_DIAG && diag_la_usertrap_count < 8)
        {
            diag_la_usertrap_count++;
            printf("[diag][usertrap] pid=%d estat=%p era=%p badv=%p\n",
                   p ? p->pid : -1, (void *)r_csr_estat(),
                   (void *)r_csr_era(), (void *)r_csr_badv());
        }
    }
    if ((r_csr_prmd() & PRMD_PPLV) == 0)
    {
        printf("#### OS COMP TEST GROUP END libcbench-musl ####\n");
        panic("usertrap: not from user mode");
    }
    /*如果是用户程序的断点，简单的跳过断点指令*/
    if (((r_csr_estat() & CSR_ESTAT_ECODE) >> 16) == 0xc)
    {
#if DEBUG_BREAK //< 想看断点就改这个宏吧
        LOG_LEVEL(LOG_DEBUG, "用户程序断点\n");
#endif
        exit(0);
        trapframe->era += 4;
        goto end;
    }
    if (((r_csr_estat() & CSR_ESTAT_ECODE) >> 16) == 0xb)
    {
        if (p->killed)
        {
            p->term_signal = p->killed;
            exit(0);
        }
        /* 系统调用 */
        trapframe->era += 4;
        intr_on();
        syscall(trapframe);
    }
    else if (((r_csr_estat() & CSR_ESTAT_ECODE) >> 16 == 0x1 ||
              (r_csr_estat() & CSR_ESTAT_ECODE) >> 16 == 0x2 ||
              (r_csr_estat() & CSR_ESTAT_ECODE) >> 16 == 0x4))
    {
        /*
         * load/store page fault or page modify exception.
         * PME (0x4) is raised by the software TLB walk when a store hits a
         * writable page whose dirty bit is clear; pagefault_handler() sets
         * D and retries, or reports a protection violation.
         * check if the page fault is caused by stack growth
         */
        if (pagefault_handler(r_csr_badv()) < 0)
        {
            p->term_signal = SIGSEGV;
            p->killed = SIGSEGV;
            exit(SIGSEGV);
        }
        // printf("usertrap():handling exception\n");
        // uint64 info = r_csr_crmd();
        // printf("usertrap(): crmd=0x%p\n", info);
        // info = r_csr_prmd();
        // printf("usertrap(): prmd=0x%p\n", info);
        // info = r_csr_estat();
        // printf("usertrap(): estat=0x%p\n", info);
        // info = r_csr_era();
        // printf("usertrap(): era=0x%p\n", info);
        // info = r_csr_ecfg();
        // printf("usertrap(): ecfg=0x%p\n", info);
        // info = r_csr_badi();
        // printf("usertrap(): badi=0x%p\n", info);
        // info = r_csr_badv();
        // printf("usertrap(): badv=0x%p\n\n", info);
        // printf("a0=%p\na1=%p\na2=%p\na3=%p\na4=%p\na5=%p\na6=%p\na7=%p\nsp=%p\n", trapframe->a0, trapframe->a1, trapframe->a2, trapframe->a3, trapframe->a4, trapframe->a5, trapframe->a6, trapframe->a7, trapframe->sp);
        // printf("p->pid=%d, p->sz=0x%p\n", p->pid, p->sz);
        // pte_t *pte = walk(p->pagetable, r_csr_badv(), 0);
        // printf("pte=%p (valid=%d, *pte=0x%p)\n", pte, *pte & PTE_V, *pte);
        // uint64 estat = r_csr_estat();
        // uint64 ecode = (estat & 0x3F0000) >> 16;
        // uint64 esubcode = (estat & 0x7FC00000) >> 22;
        // handle_exception(ecode, esubcode);
        // LOG_LEVEL(3, "\n       era=%p\n       badi=%p\n       badv=%p\n       crmd=%x\n", r_csr_era(), r_csr_badi(), r_csr_badv(), r_csr_crmd());
        // panic("usertrap\n");
    }
    else if ((which_dev = devintr()) != 0)
    {
#if DEBUG
        printf("中断类型: %d\n", which_dev);
#endif
    }
    else
    {
        printf("usertrap():handling exception\n");
        uint64 info = r_csr_crmd();
        printf("usertrap(): crmd=0x%p\n", info);
        info = r_csr_prmd();
        printf("usertrap(): prmd=0x%p\n", info);
        info = r_csr_estat();
        printf("usertrap(): estat=0x%p\n", info);
        info = r_csr_era();
        printf("usertrap(): era=0x%p\n", info);
        info = r_csr_ecfg();
        printf("usertrap(): ecfg=0x%p\n", info);
        info = r_csr_badi();
        printf("usertrap(): badi=0x%p\n", info);
        info = r_csr_badv();
        printf("usertrap(): badv=0x%p\n\n", info);
        
        // 添加更详细的调试信息
        printf("trapframe->era=0x%p\n", trapframe->era);
        printf("trapframe values:\n");
        printf("a0=%p\na1=%p\na2=%p\na3=%p\na4=%p\na5=%p\na6=%p\na7=%p\nsp=%p\n", trapframe->a0, trapframe->a1, trapframe->a2, trapframe->a3, trapframe->a4, trapframe->a5, trapframe->a6, trapframe->a7, trapframe->sp);
        printf("p->pid=%d, p->sz=0x%p\n", p->pid, p->sz);
        
        // 检查era是否为0，这是一个关键的异常情况
        if (r_csr_era() == 0 || trapframe->era == 0) {
            printf("CRITICAL: era is 0! This indicates a jump to NULL pointer.\n");
            printf("Process information:\n");
            printf("  pid=%d, tid=%d\n", p->pid, p->main_thread ? p->main_thread->tid : -1);
            printf("  kstack=0x%p, pagetable=0x%p\n", p->kstack, p->pagetable);
            // 打印VMA信息以帮助调试
            if (p->vma) {
                struct vma *vma = p->vma->next;
                int vma_count = 0;
                printf("  VMA list:\n");
                while (vma != p->vma && vma_count < 10) { // 限制打印数量防止无限循环
                    printf("    VMA[%d]: addr=0x%p-0x%p, type=%d, perm=0x%x\n", 
                           vma_count, vma->addr, vma->end, vma->type, vma->perm);
                    vma = vma->next;
                    vma_count++;
                }
            }
        }
        
        uint64 badv = r_csr_badv();
        /* PPI/INE report BADV as zero on 2K1000.  ERA is then the only
         * address that identifies the rejected user leaf. */
        uint64 fault_va = badv ? badv : trapframe->era;
        if (fault_va < MAXVA)
        {
            pte_t *pte = walk(p->pagetable, fault_va, 0);
            printf("[diag][fault-pte] va=%p pte=%p\n", fault_va,
                   pte ? (void *)*pte : NULL);
            diag_walk_chain(p->pagetable, fault_va);
            if (pte)
                printf("pte=%p (valid=%d, *pte=0x%p)\n", pte, *pte & PTE_V, *pte);
            else
                printf("pte=(nil)\n");
        }
        else
        {
            printf("fault_va %p exceeds MAXVA %p\n", fault_va, MAXVA);
        }
        printf("p->pid=%d, p->sz=0x%p\n", p->pid, p->sz);
        uint64 estat = r_csr_estat();
        uint64 ecode = (estat & 0x3F0000) >> 16;
        uint64 esubcode = (estat & 0x7FC00000) >> 22;
        handle_exception(ecode, esubcode);
        LOG_LEVEL(3, "\n       era=%p\n       badi=%p\n       badv=%p\n       crmd=%x\n", r_csr_era(), r_csr_badi(), r_csr_badv(), r_csr_crmd());
        p->term_signal = SIGSEGV;
        p->killed = SIGSEGV;
        exit(SIGSEGV);
    }
    if (which_dev == 2)
    {
        yield();
        p->utime++;
    }
end:
    hsai_usertrapret();
#endif
}

#define VIRTIO0_IRQ 1
/**
 * @brief 判断中断类型
 *
 * @return int 1是外部中断(读磁盘)，2是时钟中断，0是错误
 */
int devintr(void)
{
#if defined RISCV
    uint64 scause = r_scause();
#if DEBUG
// printf("devintr: scause=0x%lx\n", scause);
#endif
    if ((scause & 0x8000000000000000L) &&
        (scause & 0xff) == 9)
    {
        // this is a supervisor external interrupt, via PLIC.

        // irq indicates which device interrupted.
        int irq = plic_claim();

        if (irq == VIRTIO0_IRQ)
        {
            virtio_disk_intr();
        }
        else if (irq)
        {
            printf("unexpected interrupt irq=%d\n", irq);
        }

        // the PLIC allows each device to raise at most one
        // interrupt at a time; tell the PLIC the device is
        // now allowed to interrupt again.
        if (irq)
            plic_complete(irq);

        return 1;
    }
    /* 时钟中断 */
    else if (scause == 0x8000000000000005L)
    {
        timer_tick();
        return 2;
    }
    else
    {
        /* 不知道的中断类型 */
        if (!(scause & 0x8UL))
            printf("unexpected interrupt scause=0x%lx\n", scause);
        return 0;
    }
#else ///< Loongarch
    uint32 estat = r_csr_estat();
    uint32 ecfg = r_csr_ecfg();

    /* CSR.ESTAT.IS & CSR.ECFG.LIE -> int_vec(13bits stand for irq type) */
    if (estat & ecfg & HWI_VEC) ///< 8个硬中断
    {
        // TODO
        printf("kerneltrap: hardware interrupt cause %x\n", estat);
        return 1;
    }
    else if (estat & ecfg & TI_VEC) ///< 定时器中断
    {
        {
            static int diag_timer_count = 0;
            if (FINAL_DEV_DIAG && diag_timer_count < 8)
            {
                diag_timer_count++;
                printf("[diag][timer-irq] estat=%x\n", estat);
            }
        }
        timer_tick();

        /* 标明已经处理中断信号 */
        w_csr_ticlr(r_csr_ticlr() | CSR_TICLR_CLR);
        return 2;
    }
    else ///< 其他未处理的中断
    {
        printf("kerneltrap: unexpected trap cause %x\n", estat);
        return 0;
    }
#endif
}

/**
 * @brief 内核态中断和异常处理函数
 *
 */
void kerneltrap(void)
{
#if defined RISCV
#if DEBUG
// printf("kerneltrap! \n");
#endif
    // while(1) ;
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    if ((which_dev = devintr()) == 0)
    {
        struct proc *p = myproc();
        struct trapframe *trapframe = p ? p->trapframe : 0;

        if (p && scause == InstructionPageFault && sepc < MAXVA)
        {
            printf("kerneltrap: user-range instruction fault, convert to SIGSEGV\n");
            printf("scause %p\n", scause);
            printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
            printf("satp=%p expected_user_satp=%p kernel_satp_saved=%p stvec=%p sstatus=%p\n",
                   r_satp(), MAKE_SATP(p->pagetable), p->trapframe->kernel_satp, r_stvec(), r_sstatus());
            printf("context ra=%p sp=%p\n", p->context.ra, p->context.sp);
            printf("thread context ra=%p sp=%p\n", p->main_thread->context.ra, p->main_thread->context.sp);
            printf("trapframe epc=%p sp=%p kernel_sp=%p a7=%p\n",
                   p->trapframe->epc, p->trapframe->sp, p->trapframe->kernel_sp, p->trapframe->a7);
            pte_t *fault_pte = walk(p->pagetable, r_sepc(), 0);
            if (fault_pte && (*fault_pte & PTE_V))
            {
                printf("fault-pte=%p V=%d R=%d W=%d X=%d U=%d raw=%p\n",
                       fault_pte,
                       !!(*fault_pte & PTE_V),
                       !!(*fault_pte & PTE_R),
                       !!(*fault_pte & PTE_W),
                       !!(*fault_pte & PTE_X),
                       !!(*fault_pte & PTE_U),
                       *fault_pte);
            }
            else
            {
                printf("fault-pte: unmapped for sepc=%p\n", r_sepc());
            }
            p->term_signal = SIGSEGV;
            p->killed = SIGSEGV;
            exit(SIGSEGV);
            panic("kerneltrap exit return");
        }

        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
        if (p == 0)
        {
            printf("kerneltrap: no current process on hart=%p cpu=%p\n", r_tp(), mycpu());
            panic("kerneltrap");
        }
        trapframe = p->trapframe;
        printf("kerneltrap: p=%p trapframe=%p main_thread=%p main_tf=%p\n",
               p,
               trapframe,
               p->main_thread,
               p->main_thread ? p->main_thread->trapframe : 0);
        if (trapframe == 0)
            panic("kerneltrap: null trapframe");
        if (p->main_thread == 0)
            panic("kerneltrap: null main_thread");
        if (p->main_thread->trapframe == 0)
            panic("kerneltrap: null main_thread trapframe");
        printf("trapframe a0=%p\na1=%p\na2=%p\na3=%p\na4=%p\na5=%p\na6=%p\na7=%p\nsp=%p\nepc=%p\n",
               trapframe->a0, trapframe->a1, trapframe->a2, trapframe->a3, trapframe->a4,
               trapframe->a5, trapframe->a6, trapframe->a7, trapframe->sp, trapframe->epc);
        printf("thread tid=%d pid=%d, p->sz=0x%p\n", p->main_thread->tid, p->pid, p->sz);
        printf("context ra=%p sp=%p\n", p->context.ra, p->context.sp);
        printf("thread context ra=%p sp=%p\n", p->main_thread->context.ra, p->main_thread->context.sp);
        printf("thread trapframe epc=%p sp=%p kernel_sp=%p\n",
               p->main_thread->trapframe->epc,
               p->main_thread->trapframe->sp,
               p->main_thread->trapframe->kernel_sp);
        panic("kerneltrap");
    }
    // 这里删去了时钟中断的代码，时钟中断使用yield

    /* 内核对应time++ */
    if (which_dev == 2 && myproc() != 0)
    {
        // printf("内核态时钟中断\n");
        myproc()->ktime++;
    }
    /* 正在运行的进程需要重新调度 */
    if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
        yield();

    // give up the CPU if this is a timer interrupt.

    // the yield() may have caused some traps to occur,
    // so restore trap registers for use by kernelvec.S's sepc instruction.
    w_sepc(sepc);
    w_sstatus(sstatus);
#else           ///< Loongarch
    int which_dev = 0;
    // ERA寄存器：异常程序计数器，记录异常发生时的指令地址。
    uint64 era = r_csr_era();
    // PRMD寄存器：记录异常发生时的特权级别、中断使能、写使能。
    uint64 prmd = r_csr_prmd();

    {
        static int diag_kt_count = 0;
        if (FINAL_DEV_DIAG && diag_kt_count < 8)
        {
            diag_kt_count++;
            printf("[diag][kerneltrap] era=%p prmd=%p estat=%p\n",
                   (void *)era, (void *)prmd, (void *)r_csr_estat());
        }
    }

    assert((prmd & PRMD_PPLV) == 0,
           "kerneltrap: not from privilege0");
    assert(intr_get() == 0,
           "kerneltrap: interrupts enabled");
    /*如果是内核的断点，简单的跳过断点指令*/
    if (((r_csr_estat() & CSR_ESTAT_ECODE) >> 16) == 0xc)
    {
#if DEBUG_BREAK //< 想看内核断点就改这个宏吧
        LOG_LEVEL(LOG_DEBUG, "内核断点\n");
#endif
        era += 4;
        goto end;
    }

    if ((which_dev = devintr()) == 0)
    {
        printf("usertrap():handling exception\n");
        uint64 info = r_csr_crmd();
        printf("usertrap(): crmd=0x%p\n", info);
        info = r_csr_prmd();
        printf("usertrap(): prmd=0x%p\n", info);
        info = r_csr_estat();
        printf("usertrap(): estat=0x%p\n", info);
        info = r_csr_era();
        printf("usertrap(): era=0x%p\n", info);
        info = r_csr_ecfg();
        printf("usertrap(): ecfg=0x%p\n", info);
        info = r_csr_badi();
        printf("usertrap(): badi=0x%p\n", info);
        info = r_csr_badv();
        printf("usertrap(): badv=0x%p\n\n", info);
        uint64 estat = r_csr_estat();
        uint64 ecode = (estat & 0x3F0000) >> 16;
        uint64 esubcode = (estat & 0x7FC00000) >> 22;
        handle_exception(ecode, esubcode);
        LOG_LEVEL(3, "\n       era=%p\n       badi=%p\n       badv=%p\n       crmd=%x\n", r_csr_era(), r_csr_badi(), r_csr_badv(), r_csr_crmd());
        panic("kerneltrap");
    }

    if (which_dev == 2 && myproc() != 0)
        myproc()->ktime++;

    /* give up the CPU if this is a timer interrupt. */
    if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
        yield();

    /*
     * the yield() may have caused some traps to occur,
     * so restore trap registers for use by kernelvec.S's instruction.
     */
    /*如果是内核的断点，简单的跳过断点指令*/
end:
    w_csr_era(era);
    w_csr_prmd(prmd);
#endif
}
