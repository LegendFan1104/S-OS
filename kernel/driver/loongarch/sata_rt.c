#if defined RISCV
// RISC-V 构建不编译本文件（与 sata.c 相同的约定）

#else

#include "types.h"
#include "print.h"
#include "defs.h"
#include "spinlock.h"
#include "string.h"
#include "buf.h"
#include "loongarch.h"

/*
 * 2K1000 板上 SATA 驱动（AHCI 1.1）——C 移植版
 * ------------------------------------------------------------------
 * 移植自 OSKernel2025-rustoswhu 的 isomorphic_drivers/src/block/ahci.rs
 * （https://gitlab.eduxiji.net/T202510486995158/OSKernel2025-rustoswhu），
 * 流程、寄存器、时序与其完全一致：
 *   AHCI::new -> GHC AE/HR -> 找端口(SUD+等链路) -> 停引擎(ST/FRE) ->
 *   分配 4 页 DMA -> 填 PRDT/命令头 -> 写 PxCLB/PxFB(32位PA) ->
 *   清 SERR -> SUD/ICC/FRE/ST 启动 -> IDENTIFY -> 轮询 PxCI 完成
 *
 * 与 Rust 版一致的关键约定：
 *   1. DMA 结构/数据缓冲放在 0x9000 一致可缓存窗口
 *      （Rust: VIRT_ADDR_START = 0x9000_0000_0000_0000），由 2K1000
 *      目录协议维护 IO DMA 一致性，全程不做任何 cacop。
 *   2. 写进 HBA 的 DMA 地址一律是 32 位物理地址，高 32 位为 0。
 *   3. 不做 COMRESET/端口复位，只停引擎后换成自己的结构再启动。
 */

/* ------------------------------ 全局状态 ------------------------------ */

static volatile uint32 *sata_rt_hba;   /* 0x8000 非缓存窗口映射的 HBA MMIO */
static uint32 sata_rt_port;            /* 使用的端口号 */
static uint64 sata_rt_nsectors = 0;    /* 512 字节扇区数 */
static int sata_rt_ready = 0;
static struct spinlock sata_rt_lock;
static int sata_rt_io_dbg = 0;         /* 实验：前几次块 IO 打印入口 */

/* ---------------------------- 窗口与 DMA 缓冲 ---------------------------- */
/* 4 页连续，物理地址 0xB0000000 起（bank1，位于内核管理内存之外）。
 * 与 Rust 版 Provider::alloc_dma 等价：返回 (VA=PA|VIRT_ADDR_START, PA)。 */
#define SATA_RT_UNCACHED 0x8000000000000000ULL   /* MMIO 用（强序非缓存） */
#define SATA_RT_CACHED   0x9000000000000000ULL   /* DMA 缓冲用（一致可缓存） */
#define SATA_RT_DMA_PA   0x00000000B0000000ULL

#define SATA_RT_RFIS_VA  (SATA_RT_CACHED | (SATA_RT_DMA_PA + 0x0000ULL))
#define SATA_RT_CL_VA    (SATA_RT_CACHED | (SATA_RT_DMA_PA + 0x1000ULL))
#define SATA_RT_CT_VA    (SATA_RT_CACHED | (SATA_RT_DMA_PA + 0x2000ULL))
#define SATA_RT_DATA_VA  (SATA_RT_CACHED | (SATA_RT_DMA_PA + 0x3000ULL))

#define SATA_RT_RFIS_PA  (SATA_RT_DMA_PA + 0x0000ULL)
#define SATA_RT_CL_PA    (SATA_RT_DMA_PA + 0x1000ULL)
#define SATA_RT_CT_PA    (SATA_RT_DMA_PA + 0x2000ULL)
#define SATA_RT_DATA_PA  (SATA_RT_DMA_PA + 0x3000ULL)

/* --------------------- HBA 寄存器（Generic Host Control） --------------------- */
#define RT_CAP   0x00
#define RT_GHC   0x04
#define RT_IS    0x08
#define RT_PI    0x0c
#define RT_VS    0x10

#define RT_GHC_AE (1U << 31)
#define RT_GHC_HR (1U << 0)

/* --------------------- 端口寄存器（3.3，端口基址 0x100 + 0x80*n） --------------------- */
#define RT_PB     0x100
#define RT_PS     0x80
#define RT_CLB    0x00
#define RT_CLBU   0x04
#define RT_FB     0x08
#define RT_FBU    0x0c
#define RT_ISR    0x10
#define RT_IE     0x14
#define RT_CMD    0x18
#define RT_TFD    0x20
#define RT_SIG    0x24
#define RT_SSTS   0x28
#define RT_SCTL   0x2c
#define RT_SERR   0x30
#define RT_SACT   0x34
#define RT_CI     0x38

#define RT_CMD_ST  (1U << 0)
#define RT_CMD_SUD (1U << 1)
#define RT_CMD_FRE (1U << 4)
#define RT_CMD_FR  (1U << 14)
#define RT_CMD_CR  (1U << 15)
#define RT_CMD_ICC (0xfU << 28)

#define HBA32(o)  (*(volatile uint32 *)((uint64)sata_rt_hba + (o)))
#define PR32(o)   (*(volatile uint32 *)((uint64)sata_rt_hba + RT_PB + \
                                        ((uint64)sata_rt_port * RT_PS) + (o)))

/* --------------------- AHCI 数据结构（4.2，与 ahci.rs 布局一致） --------------------- */
struct rt_cmd_hdr {
    uint16 flags;            /* +0: CFL | ATAPI | W | P | R | B | C | PMP */
    uint16 prdtl;            /* +2: PRD 条目数 */
    uint32 prd_byte_count;   /* +4: PRD 传输字节数 */
    uint64 ctba;             /* +8: 命令表地址 */
    uint32 rsvd[4];          /* +16 */
};                           /* 32 字节 */

struct rt_prdt {
    uint64 dba;              /* +0: 数据基地址 */
    uint32 rsvd;             /* +8 */
    uint32 flags_size;       /* +12: bit31 IOC, bit30-22 保留, bit21-0 字节数-1 */
};                           /* 16 字节 */

struct rt_cmd_tbl {
    uint8 cfis[64];          /* +0x00: 20 字节 H2D FIS + 填充 */
    uint8 acmd[16];          /* +0x40 */
    uint8 rsvd[48];          /* +0x50 */
    struct rt_prdt prdt[1];  /* +0x80 */
};

/* ------------------------------ PCI 配置 ------------------------------ */
/* ahci.rs 的 PCI_CONFIG_ADDRESS = 0x8000_00FE_0000_0000（ECAM 基址 0xFE000000）；
 * 本工程既有驱动在 0x1a000000 验证过，两个基址都试。 */
#define RT_PCI_ECAM_PHYS 0xFE000000ULL
#define RT_PCI_ECAM_FB   0x1a000000ULL
#define RT_PCI_VENDOR    0x0014
#define RT_PCI_DEVICE    0x7a08
#define RT_PCI_CLASS_SATA 0x010601U

static volatile uint32 *sata_rt_cfg;   /* SATA 设备的配置头基址 */

static uint16 rt_cfg_read16(uint32 off)
{
    volatile uint32 *p = sata_rt_cfg + ((off & ~3U) >> 2);
    uint32 v = *p;
    return (uint16)((off & 2U) ? (v >> 16) : v);
}

static uint32 rt_cfg_read32(uint32 off)
{
    return *(sata_rt_cfg + (off >> 2));
}

static void rt_cfg_write16(uint32 off, uint16 val)
{
    volatile uint32 *p = sata_rt_cfg + ((off & ~3U) >> 2);
    uint32 v = *p;
    if (off & 2U)
        v = (v & 0xffffU) | ((uint32)val << 16);
    else
        v = (v & 0xffff0000U) | val;
    *p = v;
    (void)*p;
}

/* 对应 ahci.rs pci_init：找到 class 01/06/01（SATA AHCI），使能
 * IO Space|MEM Space|Bus Mastering|Special Cycles（0x40f，再 0xf）。 */
static int rt_pci_find(void)
{
    const uint64 bases[2] = { RT_PCI_ECAM_PHYS, RT_PCI_ECAM_FB };
    int bi, dev;

    for (bi = 0; bi < 2; bi++)
    {
        volatile uint32 *ecam =
            (volatile uint32 *)(uintptr_t)(SATA_RT_UNCACHED | bases[bi]);
        for (dev = 0; dev < 32; dev++)
        {
            volatile uint32 *cfg = ecam + ((((uint32)dev << 11) | (0U << 8)) >> 2);
            uint32 id = cfg[0];
            uint32 cls = cfg[2];

            if ((id & 0xffffU) == RT_PCI_VENDOR &&
                ((id >> 16) & 0xffffU) == RT_PCI_DEVICE)
            {
                sata_rt_cfg = (volatile uint32 *)cfg;
                goto found;
            }
            if ((cls >> 8) == RT_PCI_CLASS_SATA)
            {
                sata_rt_cfg = (volatile uint32 *)cfg;
                goto found;
            }
        }
    }
    return -1;

found:
    /* ahci.rs enable()：先 orig|0x40f，再 orig|0xf */
    {
        uint16 orig = rt_cfg_read16(0x04);
        rt_cfg_write16(0x04, (uint16)(orig | 0x40f));
        rt_cfg_write16(0x04, (uint16)(orig | 0xf));
    }
    /* BAR0（ahci.rs 取 dev.bars[0]；本板 Linux 亦取 BAR0，映射到 0x400e0000） */
    {
        uint32 bar = rt_cfg_read32(0x10);
        uint64 bar_phys;
        int i;

        if (bar != 0 && !(bar & 1U))
        {
            bar_phys = (uint64)(bar & ~0xfU);
            if (bar & 0x4U)
            {
                uint32 hi = rt_cfg_read32(0x14);
                bar_phys |= (uint64)(hi & 0xfffffff0U) << 32;
            }
            sata_rt_hba =
                (volatile uint32 *)(uintptr_t)(SATA_RT_UNCACHED | bar_phys);
            if (HBA32(RT_CAP) != 0 && HBA32(RT_CAP) != 0xffffffffU)
                return 0;
        }
        /* BAR0 无效时兜底扫其他内存 BAR */
        for (i = 1; i <= 5; i++)
        {
            bar = rt_cfg_read32(0x10 + 4U * i);
            if (bar == 0 || (bar & 1U))
                continue;
            bar_phys = (uint64)(bar & ~0xfU);
            if (bar & 0x4U)
            {
                uint32 hi = rt_cfg_read32(0x10 + 4U * (i + 1));
                bar_phys |= (uint64)(hi & 0xfffffff0U) << 32;
            }
            sata_rt_hba =
                (volatile uint32 *)(uintptr_t)(SATA_RT_UNCACHED | bar_phys);
            if (HBA32(RT_CAP) != 0 && HBA32(RT_CAP) != 0xffffffffU)
                return 0;
        }
    }
    return -1;
}

/* ------------------------------ 工具函数 ------------------------------ */

/* 对应 Rust core::hint::spin_loop 的粗略延时循环 */
static void rt_spin(int n)
{
    volatile int i;
    for (i = 0; i < n; i++)
        ;
}

/* 对应 AHCIPort::spin_on_slot：忙等 PxCI 对应槽位清 0。
 * 与 Rust 版一致不设超时；仅在长时间未完成时打印一次提示，便于定位。 */
static void rt_spin_on_slot(int slot)
{
    uint64 cnt = 0;
    for (;;)
    {
        uint32 ci = PR32(RT_CI);
        if (!(ci & (1U << slot)))
            break;
        if ((++cnt & 0x3fffffffUL) == 0)
            printf("[sata] still waiting slot %d (CI=0x%x TFD=0x%x)\n",
                   slot, PR32(RT_CI), PR32(RT_TFD));
    }
}

/* 对应 AHCIGenericHostControl::enable_ahci */
static void rt_enable_ahci(void)
{
    int i;
    HBA32(RT_GHC) |= RT_GHC_AE;
    for (i = 0; i < 1000; i++)
    {
        if (HBA32(RT_GHC) & RT_GHC_AE)
            break;
        HBA32(RT_GHC) |= RT_GHC_AE;
    }
}

/* 对应 AHCIGenericHostControl::enable（ref: Linux ahci_reset_controller） */
static void rt_ghc_enable(void)
{
    rt_enable_ahci();
    if (!(HBA32(RT_GHC) & RT_GHC_HR))
    {
        HBA32(RT_GHC) |= RT_GHC_HR;
        (void)HBA32(RT_GHC);                 /* flush */
        while (HBA32(RT_GHC) & RT_GHC_HR)    /* 等硬件清 HR */
            ;
        rt_enable_ahci();
    }
}

/* 对应 AHCI::new 里的端口查找：
 *   置 SUD -> 延时 -> 轮询 SSTS 直到 DET=3 且 IPM=1（循环满 3M 次） */
static int rt_find_port(void)
{
    uint32 cap = HBA32(RT_CAP);
    uint32 pi = HBA32(RT_PI);
    int nports = (int)(cap & 0x1fU) + 1;
    int i;

    for (i = 0; i < nports; i++)
    {
        int ok = 0, k;
        if (!(pi & (1U << i)))
            continue;
        sata_rt_port = (uint32)i;
        PR32(RT_CMD) |= RT_CMD_SUD;
        rt_spin(3000000);
        for (k = 0; k < 3000000; k++)
        {
            uint32 ssts = PR32(RT_SSTS);
            int ipm_active = ((ssts >> 8) & 0xf) == 1;
            int det_present = (ssts & 0xf) == 3;
            if (ipm_active && det_present)
                ok = 1;
        }
        if (ok)
            return 0;
    }
    return -1;
}

/* 对应 AHCI::new 里 ahci_stop_engine/ahci_stop_fis_rx：
 *   关 ST 等 CR 清 0；关 FRE 等 FR 清 0。
 * （原 Rust 代码两处 `while x | (1<<n) == 1` 的 `|` 系笔误，
 *  按注释意图 LIST_ON/FIS_ON 用 `&` 实现。） */
static void rt_stop_engine(void)
{
    uint32 cmd;

    cmd = PR32(RT_CMD);
    cmd &= ~RT_CMD_ST;
    PR32(RT_CMD) = cmd;
    while (PR32(RT_CMD) & RT_CMD_CR)
        ;

    cmd = PR32(RT_CMD);
    cmd &= ~RT_CMD_FRE;
    PR32(RT_CMD) = cmd;
    while (PR32(RT_CMD) & RT_CMD_FR)
        ;
}

/* 对应 AHCI::new 后半段：DMA 结构 + 端口启动 */
static int rt_port_start(void)
{
    struct rt_cmd_tbl *ct = (struct rt_cmd_tbl *)(uintptr_t)SATA_RT_CT_VA;
    struct rt_cmd_hdr *cl = (struct rt_cmd_hdr *)(uintptr_t)SATA_RT_CL_VA;
    uint32 cmd;

    /* 4 页 DMA 清零（等价 Rust 版新分配页面后的初始状态） */
    memset((void *)(uintptr_t)SATA_RT_RFIS_VA, 0, 0x100);
    memset((void *)(uintptr_t)SATA_RT_CL_VA, 0, 0x400);
    memset((void *)(uintptr_t)SATA_RT_CT_VA, 0, 0x100);
    memset((void *)(uintptr_t)SATA_RT_DATA_VA, 0, 512);

    /* 先填 PRDT，再填命令头（顺序与 ahci.rs 一致） */
    ct->prdt[0].dba = SATA_RT_DATA_PA;
    ct->prdt[0].flags_size = 512 - 1;

    cl[0].ctba = SATA_RT_CT_PA;
    cl[0].prdtl = 1;
    cl[0].prd_byte_count = 0;
    cl[0].flags = 5;                    /* cfl=5（ahci.rs 里改过的值） */

    /* PxCLB/PxFB 写 64 位物理地址（PA 为 32 位，高 32 位为 0） */
    PR32(RT_CLB) = (uint32)(SATA_RT_CL_PA & 0xffffffffULL);
    PR32(RT_CLBU) = (uint32)(SATA_RT_CL_PA >> 32);
    PR32(RT_FB) = (uint32)(SATA_RT_RFIS_PA & 0xffffffffULL);
    PR32(RT_FBU) = (uint32)(SATA_RT_RFIS_PA >> 32);

    /* 清错误（ref: Linux ahci_power_up 前） */
    PR32(RT_SERR) = 0xffffffffU;

    /* ref: Linux ahci_power_up：SUD 置位 */
    PR32(RT_CMD) |= RT_CMD_SUD;
    /* ICC = Active */
    PR32(RT_CMD) = (PR32(RT_CMD) & ~RT_CMD_ICC) | (1U << 28);
    /* ref: Linux ahci_start_fis_rx：FRE 置位 + flush */
    PR32(RT_CMD) |= RT_CMD_FRE;
    (void)PR32(RT_CMD);
    /* ref: Linux ahci_start_engine：ST 置位 + flush + 等 ST */
    PR32(RT_CMD) |= RT_CMD_ST;
    (void)PR32(RT_CMD);
    while (!(PR32(RT_CMD) & RT_CMD_ST))
        ;

    if (PR32(RT_SSTS) == 0)
    {
        printf("[sata] port is not connected to external drive?\n");
        return -1;
    }

    cmd = PR32(RT_CMD);
    printf("[sata] port %d started (PxCMD=0x%x SSTS=0x%x)\n",
           (uint32)sata_rt_port, cmd, PR32(RT_SSTS));
    return 0;
}

/* ------------------------------ IDENTIFY ------------------------------ */

static int rt_identify(void)
{
    struct rt_cmd_tbl *ct = (struct rt_cmd_tbl *)(uintptr_t)SATA_RT_CT_VA;
    struct rt_cmd_hdr *cl = (struct rt_cmd_hdr *)(uintptr_t)SATA_RT_CL_VA;
    volatile uint16 *id = (volatile uint16 *)(uintptr_t)SATA_RT_DATA_VA;
    uint64 lba48, lba28;

    /* 7.15 IDENTIFY DEVICE - ECh, PIO Data-In（字段与 ahci.rs 一致） */
    ct->cfis[0] = 0x27;                  /* FIS_REG_H2D */
    ct->cfis[1] = 1 << 7;                /* C */
    ct->cfis[2] = 0xEC;                  /* CMD_IDENTIFY_DEVICE */
    ct->cfis[12] = 1;                    /* sector_count = 1 */
    ct->cfis[13] = 0;

    cl[0].flags = 5;

    printf("[sata] before spin on slot\n");
    PR32(RT_CI) = 1;
    printf("[sata] issue command done\n");
    rt_spin_on_slot(0);
    printf("[sata] after spin\n");

    lba48 = (uint64)id[100] | ((uint64)id[101] << 16) |
            ((uint64)id[102] << 32) | ((uint64)id[103] << 48);
    lba28 = (uint32)id[60] | ((uint32)id[61] << 16);
    sata_rt_nsectors = lba48 ? lba48 : lba28;
    if (sata_rt_nsectors == 0)
        return -1;

    printf("[sata] disk identified: %d sectors (%d MB)\n",
           (uint32)sata_rt_nsectors, (uint32)(sata_rt_nsectors / 2048));
    return 0;
}

/* ------------------------------ 读写块 ------------------------------ */

/* 对应 SATAFISRegH2D + set_lba；读/写都用 LBA48、1 扇区 */
static void rt_cmd_fis(uint8 cmd, uint64 lba)
{
    struct rt_cmd_tbl *ct = (struct rt_cmd_tbl *)(uintptr_t)SATA_RT_CT_VA;
    uint8 *f = ct->cfis;

    memset(f, 0, 20);
    f[0] = 0x27;                         /* FIS_REG_H2D */
    f[1] = 1 << 7;                       /* C */
    f[2] = cmd;
    f[4] = (uint8)(lba >> 0);
    f[5] = (uint8)(lba >> 8);
    f[6] = (uint8)(lba >> 16);
    f[7] = 0x40;                         /* dev_head: LBA */
    f[8] = (uint8)(lba >> 24);
    f[9] = (uint8)(lba >> 32);
    f[10] = (uint8)(lba >> 40);
    f[12] = 1;                           /* sector_count = 1 */
    f[13] = 0;
    f[15] = 0x80;                        /* control: LBA48 */
}

/* 对应 AHCI::read_block：READ DMA EXT - 25h */
static void rt_read_block(uint64 block_id, uint8 *dst)
{
    struct rt_cmd_hdr *cl = (struct rt_cmd_hdr *)(uintptr_t)SATA_RT_CL_VA;

    cl[0].flags = 5;
    rt_cmd_fis(0x25, block_id);
    PR32(RT_CI) = 1;
    rt_spin_on_slot(0);
    memmove(dst, (void *)(uintptr_t)SATA_RT_DATA_VA, 512);
}

/* 对应 AHCI::write_block：WRITE DMA EXT - 35h，flags 加 WRITE 位 */
static void rt_write_block(uint64 block_id, const uint8 *src)
{
    struct rt_cmd_hdr *cl = (struct rt_cmd_hdr *)(uintptr_t)SATA_RT_CL_VA;

    cl[0].flags = 5 | (1 << 6);          /* WRITE */
    memmove((void *)(uintptr_t)SATA_RT_DATA_VA, src, 512);
    rt_cmd_fis(0x35, block_id);
    PR32(RT_CI) = 1;
    rt_spin_on_slot(0);
}

/* ------------------------------ 对外接口 ------------------------------ */

void sata_init(void)
{
    initlock(&sata_rt_lock, "sata_rt");
    sata_rt_hba = NULL;
    sata_rt_ready = 0;

    /* 对应 pci_init + AHCI::new(pa | 0x8000_0000_0000_0000) */
    if (rt_pci_find() != 0)
    {
        printf("[sata] Loongson SATA controller not found\n");
        return;
    }
    printf("[sata] HBA at 0x%x (CAP=0x%x)\n",
           (uint32)((uint64)sata_rt_hba & 0xffffffffULL), HBA32(RT_CAP));

    rt_ghc_enable();
    rt_spin(3000000);

    if (rt_find_port() != 0)
    {
        printf("[sata] no usable port\n");
        return;
    }

    rt_stop_engine();
    if (rt_port_start() != 0)
        return;
    if (rt_identify() != 0)
    {
        printf("[sata] identify failed\n");
        return;
    }

    sata_rt_ready = 1;
    printf("[sata] ready: port %d, %d sectors\n",
           (uint32)sata_rt_port, (uint32)sata_rt_nsectors);

    /* 实验：初始化完成后自测 DMA READ（扇区 0），验证 fs 读取路径 */
    {
        static uint8 testbuf[512];
        uint32 *p = (uint32 *)testbuf;
        rt_read_block(0, testbuf);
        printf("[sata] TEST READ sector0: %x %x %x %x %x %x %x %x\n",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
}

/* 块设备读写入口（bio.c 调用，与 la_virtio_disk_rw 同签名）。
 * b->blockno 是 BSIZE(4096) 块号，换算成 512 字节扇区；每命令 1 扇区，
 * 一个 4K 块循环 8 次（与 ahci.rs read_block/write_block 一致）。 */
void la_sata_disk_rw(struct buf *b, int write)
{
    uint64 sector = (uint64)b->blockno * (BSIZE / 512);
    int i;

    if (!sata_rt_ready)
        panic("sata: disk not ready");

    if (sata_rt_io_dbg < 8)
    {
        sata_rt_io_dbg++;
        printf("[sata] rw blockno=%d write=%d sector=%lld\n",
               b->blockno, write, (unsigned long long)sector);
    }

    acquire(&sata_rt_lock);
    for (i = 0; i < BSIZE / 512; i++)
    {
        if (write)
            rt_write_block(sector + (uint64)i, b->data + i * 512);
        else
            rt_read_block(sector + (uint64)i, b->data + i * 512);
    }
    release(&sata_rt_lock);
}

#endif
