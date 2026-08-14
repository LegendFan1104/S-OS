#if defined RISCV
// RISC-V 构建不编译本文件（与 virtio_disk.c 相同的约定）

#else

#include "types.h"
#include "print.h"
#include "defs.h"
#include "spinlock.h"
#include "string.h"
#include "buf.h"
#include "loongarch.h"

/* r_time() 依赖的稳定计数器频率（kernel/timer.c 已校准，2K1000 上为 100MHz） */
extern uint64 timer_freq;

/*
 * 2K1000 板上 SATA 驱动（AHCI 1.3）
 * ------------------------------------------------------------------
 * 移植自星云板官方 U-Boot（open-loongarch/u-boot，loongson_2k1000_dp_defconfig）
 * 的 drivers/ata/ahci.c，寄存器偏移/位定义与 U-Boot include/ahci.h 一致；
 * 流程对应：
 *   ahci_reset / ahci_host_init / ahci_link_up / wait_spinup /
 *   ahci_port_start / ahci_fill_sg / ahci_fill_cmd_slot /
 *   ahci_device_data_io
 * 采用轮询完成（PxCI 位清 0），不依赖中断。
 *
 * 与官方流程的差异（均为 2K1000 真板必需）：
 * 1. 接管模式：U-Boot 已经把 SATA 初始化好（IDENTIFY 成功、链路已建），
 *    内核不再做 HBA 热复位和 PHY 重协商，而是停掉端口引擎、换成自己的
 *    PxCLB/PxFB 后重启，避免真板在热复位后 PIO 数据阶段出 DIAG.X 错误。
 *    只有检测不到可用端口时才走标准的 ahci_host_init 全量初始化。
 * 2. 缓存一致性：CPU 通过 DMW0（0x9000 一致可缓存窗口）访问 DMA 结构，
 *    与 SATA 控制器的物理地址视图相同；下发命令前用 cacop 0x11
 *    （D-cache Hit Writeback+Invalidate，对应 Linux cacheops.h 的
 *    Hit_Writeback_Inv_D）写回，读命令完成后同样失效，防止读到陈旧行。
 * 3. DMA 地址：按《龙芯 2K1000LA 处理器用户手册》6.4 节，IO DMA 请求
 *    经 IO 互连地址窗口路由，缺省不做地址转换，DMA 地址必须是内存控制器
 *    可解码的物理地址（32 位 PA：0x00000000-0x0FFFFFFF 与
 *    0x90000000-0xFFFFFFFF 两段 DDR）。0x9000 前缀只是 CPU 的 DMW
 *    虚拟地址，写进 PxCLB/PRDT 会在数据突发时产生 HBFS（PxIS bit27）。
 *    因此 PxCLB/PxFB/命令表/SG 全部写 32 位 PA，高 32 位写 0。
 *
 * 2K1000 的 SATA 控制器是 SoC 内部 PCI 设备：
 *   vendor = 0x0014 (Loongson), device = 0x7a08, class = 0x010601
 *   Linux 设备树: /bus@10000000/pci@1a000000/sata@8,0 (device 8, fn 0)
 *   PCI 配置空间(ECAM)物理基址: 0x1a000000（可用 -DSATA_PCIE_ECAM_PHYS= 覆盖）
 *   AHCI HBA 的 ABAR 在 PCI BAR5（U-Boot 取 BAR5；Linux 对 0x7a08 取 BAR0，
 *   2K1000 上两者都映射到 0x400e0000），经 DMW1 (0x8000...) 非缓存窗口访问。
 */

#define LA_DMW0_MASK 0x9000000000000000ULL
#define LA_DMW1_MASK 0x8000000000000000ULL

#ifndef SATA_PCIE_ECAM_PHYS
#define SATA_PCIE_ECAM_PHYS 0x1a000000UL
#endif

#define LS2K_SATA_VENDOR 0x0014
#define LS2K_SATA_DEVICE 0x7a08
#define LS2K_SATA_DEV    8
#define LS2K_SATA_FN     0

/* ---- AHCI 全局寄存器（U-Boot/Linux ahci.h 一致） ---- */
#define HOST_CAP        0x00
#define HOST_CTL        0x04
#define HOST_IRQ_STAT   0x08
#define HOST_PORTS_IMPL 0x0c
#define HOST_VERSION    0x10
#define HOST_CAP2       0x24

#define HOST_RESET    (1U << 0)   /* 控制器复位，自清 */
#define HOST_IRQ_EN   (1U << 1)
#define HOST_AHCI_EN  (1U << 31)

/* ---- AHCI 端口寄存器 ---- */
#define PORT_BASE      0x100
#define PORT_STRIDE    0x80

#define PORT_LST_ADDR      0x00
#define PORT_LST_ADDR_HI   0x04
#define PORT_FIS_ADDR      0x08
#define PORT_FIS_ADDR_HI   0x0c
#define PORT_IRQ_STAT      0x10
#define PORT_IRQ_MASK      0x14
#define PORT_CMD           0x18
#define PORT_TFDATA        0x20
#define PORT_SIG           0x24
#define PORT_SCR_STAT      0x28
#define PORT_SCR_CTL       0x2c
#define PORT_SCR_ERR       0x30
#define PORT_SACT          0x34
#define PORT_CMD_ISSUE     0x38

#define PORT_CMD_START      (1U << 0)
#define PORT_CMD_SPIN_UP    (1U << 1)
#define PORT_CMD_POWER_ON   (1U << 2)
#define PORT_CMD_FIS_RX     (1U << 4)
#define PORT_CMD_FIS_ON     (1U << 14)
#define PORT_CMD_LIST_ON    (1U << 15)
#define PORT_CMD_ICC_ACTIVE (1U << 28)

#define PORT_IRQ_TF_ERR     (1U << 30)

#define PORT_SCR_STAT_DET_MASK   0x3U
#define PORT_SCR_STAT_DET_COMINIT 0x1U
#define PORT_SCR_STAT_DET_PHYRDY 0x3U

#define ATA_BUSY 0x80U
#define ATA_DRQ  0x08U

#define ATA_CMD_ID_ATA     0xEC
#define ATA_CMD_READ_EXT   0x25
#define ATA_CMD_WRITE_EXT  0x35
#define ATA_SECT_SIZE      512

#define AHCI_MAX_SG       56
#define AHCI_MAX_CMD_SLOT 32
#define AHCI_CMD_SLOT_SZ  32
#define AHCI_RX_FIS_SZ    256
#define AHCI_CMD_TBL_HDR  0x80
#define MAX_DATA_BYTE_COUNT (4 * 1024 * 1024)

/* U-Boot ahci.c 中的等待时间（毫秒） */
#define SATA_WAIT_LINKUP_MS  200
#define SATA_WAIT_SPINUP_MS  20000
#define SATA_WAIT_DATAIO_MS  10000

#define SATA_IDENTIFY_RETRY  3

/* 只使用命令槽 0，一次一条命令 */
#define SATA_SLOT 0

struct ahci_cmd_hdr {
    uint32 opts;
    uint32 status;
    uint32 tbl_addr;
    uint32 tbl_addr_hi;
    uint32 rsvd[4];
};

struct ahci_sg {
    uint32 addr;
    uint32 addr_hi;
    uint32 flags_size;
};

struct ahci_cmd_tbl {
    uint8 cfis[AHCI_CMD_TBL_HDR];
    struct ahci_sg sg[1];
};

static volatile uint32 *sata_hba;   /* AHCI HBA，DMW1 非缓存别名 */
static volatile uint32 *sata_ecam;  /* PCI 配置空间，DMW1 非缓存别名 */
static uint32 sata_cfg_base;        /* 找到的设备的配置偏移 */
static int sata_ready = 0;
static int sata_port = 0;
static uint64 sata_nsectors = 0;    /* 512 字节扇区数 */
static struct spinlock sata_lock;
static int sata_trace_link = 0;     /* 链路采样：IDENTIFY 首次下发时置 1 */

/* 官方 U-Boot 在 LoongArch 上 flush_dcache_range 是空操作却能正常跑
 * SATA；我们把 DMA 结构放在非缓存窗口（0x8000）后，CPU 访问直接落
 * DRAM，同样不需要任何 cacop。置 1 跳过全部缓存维护。 */
static int sata_no_cache_ops = 1;

/* U-Boot 遗留端口环境（连续运行的引擎，从未停止过）：
 * U-Boot 的所有成功 SATA 操作（含板上实测 scsi read）都跑在这个引擎上；
 * 我们把命令写进它的槽位（经非缓存窗口保证落到 DRAM），不重启引擎。 */
static int sata_uboot_mode = 0;     /* 1 = I/O 全部走 U-Boot 引擎/槽位 */
static uint64 sata_uboot_clb = 0;   /* U-Boot PxCLB（0x9000 窗口视图） */
static uint64 sata_uboot_ct  = 0;   /* U-Boot slot0 命令表（0x9000 视图） */

/* 与 HBA 共享的 DMA 结构，固定在 bank1（物理 0xB0000000），经 DMW1
 * 非缓存窗口（0x8000_0000_Bxxx_xxxx）访问。
 * 板上实测结论：0x9000 缓存窗口在真机上压着旧缓存行（CPU 写入落 DRAM
 * 但缓存视图仍是旧值，HBA 经 DMA 读到旧命令表，命令根本不执行），所以
 * DMA 结构必须走 0x8000 非缓存窗口；数据地址则按 U-Boot 实测成功的
 * 64 位 0x9000 前缀格式（高32位 0x90000000）写入 PRD/命令表/PxCLB。
 * 0xB0000000 高于内核 buddy 管理的 mem_end（0xaa23c000），不会冲突。
 * 布局（全部 4KB 对齐，规避 DWC 核可能的缓冲对齐要求）：
 *   cmdlist@+0x0000(1KB) fis@+0x1000(256B)
 *   cmdtbl@+0x1100(144B) dma@+0x2000(4KB) ident@+0x3000(512B) */
#define SATA_DMA_BASE 0x80000000B0000000ULL

#define SATA_CMDLIST_VA (SATA_DMA_BASE + 0x0000ULL)
#define SATA_FIS_VA     (SATA_DMA_BASE + 0x1000ULL)
#define SATA_CMDTBL_VA  (SATA_DMA_BASE + 0x1100ULL)
#define SATA_DMA_VA     (SATA_DMA_BASE + 0x2000ULL)
#define SATA_IDENT_VA   (SATA_DMA_BASE + 0x3000ULL)

static uint8 *const sata_cmdlist = (uint8 *)SATA_CMDLIST_VA;
static uint8 *const sata_fis     = (uint8 *)SATA_FIS_VA;
static uint8 *const sata_cmdtbl  = (uint8 *)SATA_CMDTBL_VA;
static uchar *const sata_dma     = (uchar *)SATA_DMA_VA;
static uchar *const sata_ident   = (uchar *)SATA_IDENT_VA;

/* 把任意窗口的 VA 转成 HBA 用的 DMA 地址。
 * 按《龙芯 2K1000LA 处理器用户手册》6.4 节：IO DMA 请求经 IO 互连
 * 地址窗口路由，缺省配置（BASE/MASK/MMAP 全 0）不做地址转换，所以
 * DMA 地址必须能被内存控制器直接解码（0x00000000-0x0FFFFFFF 和
 * 32 位 0x90000000-0xFFFFFFFF 两段 DDR，或 0x100000000-0x1FFFFFFFF
 * 的 4G 窗口）。0x8000/0x9000/A000 前缀只是 CPU 的 DMW 直接映射
 * 虚拟地址（清 60~63 位得 PA），不是设备 DMA 地址；把 0x9000 前缀
 * 地址写进 PRDT 会导致数据阶段 PxIS=0x08000000（HBFS，Host Bus
 * Fatal Error）。因此这里取低 32 位物理地址，与 U-Boot scsi read
 * （virt_to_phys 后的 32 位 PA）以及 Linux 标准 ahci 驱动一致。 */
static inline uint64 sata_dma_addr(uint64 va)
{
    return (uint64)(uint32)va;
}

/* 任意窗口 VA 的 0x9000 一致可缓存视图（仅用于 CPU 侧的 cacop 等
 * 缓存操作，不能当作 DMA 地址写入 HBA）。 */
static inline uint64 sata_cached_view(uint64 va)
{
    return LA_DMW0_MASK | (va & 0xffffffffffffULL);
}

#define HBA32(off) (*(volatile uint32 *)((uint64)sata_hba + (off)))
#define PORT32(off) \
    (*(volatile uint32 *)((uint64)sata_hba + PORT_BASE + \
                          ((uint64)sata_port * PORT_STRIDE) + (off)))

/* 与 U-Boot writel_with_flush 等价：写后读回，保证 MMIO 写完成 */
static inline void sata_writel(volatile uint32 *reg, uint32 val)
{
    *reg = val;
    (void)*reg;
}

/* ------------------------------------------------------------------ */
/* PCI 配置空间访问                                                    */
/* ------------------------------------------------------------------ */

static uint32 sata_cfg_read32(uint32 off)
{
    return *(volatile uint32 *)((uint64)sata_ecam + sata_cfg_base + off);
}

static uint16 sata_cfg_read16(uint32 off)
{
    volatile uint32 *p =
        (volatile uint32 *)((uint64)sata_ecam + sata_cfg_base + (off & ~3U));
    uint32 v = *p;
    return (uint16)((off & 2U) ? (v >> 16) : v);
}

static void sata_cfg_write16(uint32 off, uint16 val)
{
    volatile uint32 *p =
        (volatile uint32 *)((uint64)sata_ecam + sata_cfg_base + (off & ~3U));
    uint32 v = *p;
    if (off & 2U)
        v = (v & 0xffffU) | ((uint32)val << 16);
    else
        v = (v & 0xffff0000U) | val;
    *p = v;
    (void)*p;
}

/* ------------------------------------------------------------------ */
/* 缓存一致性与延时                                                    */
/* ------------------------------------------------------------------ */

/* cacop：hint 编码为 op[4:3] | cache_leaf[2:0]（Linux
 * arch/loongarch/include/asm/cacheops.h）：
 *   Hit_Writeback_Inv = 0x10（op[4:3]=2，命中类操作）
 *   Cache_LEAF1 (D-cache) = 0x01
 *   Hit_Writeback_Inv_D = 0x10 | 0x01 = 0x11
 * 即 D-cache 命中写回并失效。下发命令前用它把命令结构/写缓冲落盘；
 * 读命令完成后也用它失效（命中失效会先写回脏行，对已经 clean 过的
 * 行是安全的）。行大小 64B。本工具链的汇编器只接受
 * cacop hint, rj, 立即数 的形式，立即数 0 编码为 rd=$zero，
 * 有效地址 = rj + rd = va。 */
#define SATA_CACOP_D_HIT_WB_INV 0x11

static inline void sata_cache_clean(uint64 va, uint64 len)
{
    if (sata_no_cache_ops)
        return;
    {
        uint64 end = va + len;
        for (va &= ~63ULL; va < end; va += 64)
            __asm__ volatile("cacop %2, %0, %1"
                             :: "r"(va), "i"(0),
                                "i"(SATA_CACOP_D_HIT_WB_INV));
    }
}
static inline void sata_cache_invalidate(uint64 va, uint64 len)
{
    if (sata_no_cache_ops)
        return;
    {
        uint64 end = va + len;
        for (va &= ~63ULL; va < end; va += 64)
            __asm__ volatile("cacop %2, %0, %1"
                             :: "r"(va), "i"(0),
                                "i"(SATA_CACOP_D_HIT_WB_INV));
    }
}

/* 非缓存窗口（0x8000...）视角：绕过 CPU 缓存直接读 DRAM，
 * 与 HBA 的 DMA 看到的物理内存一致。用于判定 L2 缓存一致性问题。 */
static inline volatile uint32 *sata_uncached_view(uint64 va)
{
    return (volatile uint32 *)(uintptr_t)(LA_DMW1_MASK |
                                          (va & 0xffffffffffffULL));
}

/* 对整个 DMA 区做 L1D+L2 写回并失效（走 0x9000 缓存别名）。
 * 目的：清除跨热复位（warm reset / go）残留在 L2 里的旧缓存行——
 * 早期版本用 0x9000 缓存窗口访问过物理 0xB0000000，那些行在复位后
 * 不消失；HBA 的 DMA 若经过 L2 探查会拿到旧数据，或 DMA 写入被旧行
 * 掩盖（ident 永远读回 0x55 的可能原因）。cacop 0x11=L1D 命中写回+
 * 失效，0x13=L2 命中写回+失效（Linux cacheops.h 同款）。 */
static void sata_flush_cached(uint64 va, uint64 len)
{
    uint64 ca = sata_cached_view(va);
    uint64 end = ca + len;

    printf("[sata] flush L2 [0x%x%x, +0x%x)\n",
           (uint32)(ca >> 32), (uint32)ca, (uint32)len);
    for (ca &= ~63ULL; ca < end; ca += 64)
    {
        __asm__ volatile("cacop %2, %0, %1"
                         :: "r"(ca), "i"(0), "i"(0x11));
        __asm__ volatile("cacop %2, %0, %1"
                         :: "r"(ca), "i"(0), "i"(0x13));
    }
}

/* 基于 rdtime 的毫秒等待（U-Boot udelay 的等价实现） */
static void sata_udelay(uint64 us)
{
    uint64 start = r_time();
    uint64 ticks = timer_freq * us / 1000000ULL;
    while (r_time() - start < ticks)
        ;
}

/* ------------------------------------------------------------------ */
/* AHCI 初始化（对应 U-Boot ahci_reset + ahci_host_init）               */
/* ------------------------------------------------------------------ */

static int sata_hba_reset(void)
{
    uint32 tmp;
    uint64 i;

    tmp = HBA32(HOST_CTL);
    if ((tmp & HOST_RESET) == 0)
        sata_writel(&HBA32(HOST_CTL), tmp | HOST_RESET);

    /* 复位必须完成，否则硬件视为故障（U-Boot 等 1 秒） */
    for (i = 0; i < 1000 && (HBA32(HOST_CTL) & HOST_RESET); i++)
        sata_udelay(1000);
    if (i == 1000)
    {
        printf("[sata] controller reset failed (0x%x)\n", HBA32(HOST_CTL));
        return -1;
    }
    return 0;
}

/* 等待 SATA 链路就绪（DET == PHYRDY），对应 U-Boot ahci_link_up */
static int sata_link_up(void)
{
    uint64 i;
    for (i = 0; i < SATA_WAIT_LINKUP_MS; i++)
    {
        if ((PORT32(PORT_SCR_STAT) & PORT_SCR_STAT_DET_MASK) ==
            PORT_SCR_STAT_DET_PHYRDY)
            return 0;
        sata_udelay(1000);
    }
    return -1;
}

/* 等待设备 spinup 完成（TFDATA 不再 busy），对应 U-Boot wait_spinup */
static int sata_wait_spinup(void)
{
    uint64 j;
    for (j = 0; j < SATA_WAIT_SPINUP_MS; j++)
    {
        uint32 tf = PORT32(PORT_TFDATA);
        if (!(tf & ATA_BUSY))
            return 0;
        sata_udelay(1000);
    }
    return -1;
}

#if 0
/* 通过 SControl 的 SPEED/DET 字段触发 PHY 复位并按指定速率重协商。
 * spd=1：1.5Gbps；spd=2：3Gbps。端口引擎（ST/FRE）不停。
 * 3Gbps 数据突发 CRC 失败时，降到 1.5Gbps 是 SATA 信号裕度的常规解法。 */
static int sata_force_link_speed(int spd)
{
    uint32 sctl = PORT32(PORT_SCR_CTL);
    uint64 j;

    sctl = (sctl & ~0xffU) | ((uint32)spd << 4) | 1U;  /* SPEED=spd, DET=1 */
    sata_writel(&PORT32(PORT_SCR_CTL), sctl);
    sata_udelay(1000);
    sctl = (sctl & ~0x0fU) | ((uint32)spd << 4);        /* DET=0 重新协商 */
    sata_writel(&PORT32(PORT_SCR_CTL), sctl);

    for (j = 0; j < SATA_WAIT_LINKUP_MS * 10; j++)
    {
        uint32 ssts = PORT32(PORT_SCR_STAT);
        if ((ssts & PORT_SCR_STAT_DET_MASK) == PORT_SCR_STAT_DET_PHYRDY)
            break;
        sata_udelay(1000);
    }
    printf("[sata] renegotiate SPEED=%d -> SSTS=0x%x (DET=%d SPD=%d IPM=%d)\n",
           spd, PORT32(PORT_SCR_STAT),
           PORT32(PORT_SCR_STAT) & 0x3,
           (PORT32(PORT_SCR_STAT) >> 4) & 0x7,
           (PORT32(PORT_SCR_STAT) >> 8) & 0xf);
    if ((PORT32(PORT_SCR_STAT) & PORT_SCR_STAT_DET_MASK) !=
        PORT_SCR_STAT_DET_PHYRDY)
        return -1;

    for (j = 0; j < SATA_WAIT_SPINUP_MS; j++)
    {
        if (!(PORT32(PORT_TFDATA) & ATA_BUSY))
            break;
        sata_udelay(1000);
    }
    PORT32(PORT_SCR_ERR) = PORT32(PORT_SCR_ERR);
    PORT32(PORT_IRQ_STAT) = PORT32(PORT_IRQ_STAT);
    return 0;
}
#endif

/* 对应 U-Boot ahci_host_init：复位 HBA、使能 AHCI、逐端口 linkup */
static int sata_host_init(void)
{
    uint32 cap_save, tmp;
    int i;

    cap_save = HBA32(HOST_CAP);
    cap_save &= ((1U << 28) | (1U << 17));
    cap_save |= (1U << 27); /* staggered spin-up */

    if (sata_hba_reset() != 0)
        return -1;

    sata_writel(&HBA32(HOST_CTL), HOST_AHCI_EN);
    HBA32(HOST_CAP) = cap_save;
    sata_writel(&HBA32(HOST_PORTS_IMPL), 0xf);

    for (i = 0; i < 32; i++)
    {
        uint32 port_map = HBA32(HOST_PORTS_IMPL);
        uint64 j;

        if (!(port_map & (1U << i)))
            continue;
        sata_port = i;

        /* 端口未激活则停掉 */
        tmp = PORT32(PORT_CMD);
        if (tmp & (PORT_CMD_LIST_ON | PORT_CMD_FIS_ON |
                   PORT_CMD_FIS_RX | PORT_CMD_START))
        {
            tmp &= ~(PORT_CMD_LIST_ON | PORT_CMD_FIS_ON |
                     PORT_CMD_FIS_RX | PORT_CMD_START);
            sata_writel(&PORT32(PORT_CMD), tmp);
            /* 规范要求 500ms/位，这里轮询等待引擎停 */
            for (j = 0; j < SATA_WAIT_SPINUP_MS; j++)
            {
                tmp = PORT32(PORT_CMD);
                if (!(tmp & (PORT_CMD_LIST_ON | PORT_CMD_FIS_ON)))
                    break;
                sata_udelay(1000);
            }
        }

        /* spin up + 等待链路 */
        tmp = PORT32(PORT_CMD);
        tmp |= PORT_CMD_SPIN_UP;
        sata_writel(&PORT32(PORT_CMD), tmp);

        if (sata_link_up() != 0)
        {
            printf("[sata] SATA link %d timeout\n", i);
            continue;
        }
        printf("[sata] port %d link up (SSTS=0x%x)\n",
               i, PORT32(PORT_SCR_STAT));

        /* 清错误并等设备就绪 */
        tmp = PORT32(PORT_SCR_ERR);
        if (tmp)
            PORT32(PORT_SCR_ERR) = tmp;
        for (j = 0; j < SATA_WAIT_SPINUP_MS; j++)
        {
            tmp = PORT32(PORT_TFDATA);
            if (!(tmp & (ATA_BUSY | ATA_DRQ)))
                break;
            sata_udelay(1000);
        }
        tmp = PORT32(PORT_SCR_STAT) & PORT_SCR_STAT_DET_MASK;
        if (tmp == PORT_SCR_STAT_DET_COMINIT)
        {
            printf("[sata] port %d COMINIT, retrying\n", i);
            i--;
            continue;
        }
        tmp = PORT32(PORT_SCR_ERR);
        if (tmp)
            PORT32(PORT_SCR_ERR) = tmp;
        tmp = PORT32(PORT_IRQ_STAT);
        if (tmp)
            PORT32(PORT_IRQ_STAT) = tmp;
        HBA32(HOST_IRQ_STAT) = (1U << i);

        /* 取第一个 link 起来的端口 */
        return 0;
    }

    sata_writel(&HBA32(HOST_CTL), HBA32(HOST_CTL) | HOST_IRQ_EN);
    printf("[sata] no link on any port\n");
    return -1;
}

/* 对应 U-Boot ahci_port_start。用于两种场景：
 * 1. 标准全量初始化（HBA 复位后首次启动端口）；
 * 2. U-Boot 环境实验失败后的回退：停掉 U-Boot 的引擎、换成内核自己的
 *    PxCLB/PxFB 再启动；PHY 链路不动。 */
static int sata_port_start(void)
{
    uint32 port_status, tmp;
    uint64 j;

    port_status = PORT32(PORT_SCR_STAT);
    if ((port_status & PORT_SCR_STAT_DET_MASK) !=
        PORT_SCR_STAT_DET_PHYRDY)
    {
        printf("[sata] no link on port %d (SSTS=0x%x)\n",
               sata_port, port_status);
        return -1;
    }

    /* 端口引擎若在运行（U-Boot 接管场景），先停掉再写 PxCLB/PxFB */
    tmp = PORT32(PORT_CMD);
    if (tmp & (PORT_CMD_LIST_ON | PORT_CMD_FIS_ON |
               PORT_CMD_FIS_RX | PORT_CMD_START))
    {
        tmp &= ~(PORT_CMD_LIST_ON | PORT_CMD_FIS_ON |
                 PORT_CMD_FIS_RX | PORT_CMD_START);
        sata_writel(&PORT32(PORT_CMD), tmp);
        for (j = 0; j < SATA_WAIT_SPINUP_MS; j++)
        {
            tmp = PORT32(PORT_CMD);
            if (!(tmp & (PORT_CMD_LIST_ON | PORT_CMD_FIS_ON)))
                break;
            sata_udelay(1000);
        }
    }

    memset(sata_cmdlist, 0, AHCI_CMD_SLOT_SZ * AHCI_MAX_CMD_SLOT);
    memset(sata_fis, 0, AHCI_RX_FIS_SZ);
    memset(sata_cmdtbl, 0, AHCI_CMD_TBL_HDR + 16);
    /* 写回 memset 结果并清掉缓存行，之后 HBA 经 DMA 写 FIS 区不会被旧缓存掩盖 */
    sata_cache_clean((uint64)sata_cmdlist, AHCI_CMD_SLOT_SZ * AHCI_MAX_CMD_SLOT);
    sata_cache_clean((uint64)sata_fis, AHCI_RX_FIS_SZ);
    sata_cache_clean((uint64)sata_cmdtbl, AHCI_CMD_TBL_HDR + 16);
    /* 实验：清掉整个 DMA 区在 L2 里的陈旧行（跨复位残留） */
    sata_flush_cached((uint64)sata_cmdlist, 0x4000);

    {
        uint64 clb_dma = sata_dma_addr((uint64)sata_cmdlist);
        uint64 fb_dma  = sata_dma_addr((uint64)sata_fis);

        /* 全部用 32 位物理地址（2K1000LA 手册 6.4 节）：PxCLB/PxFB 低
         * 32 位写 PA，高 32 位写 0。U-Boot 留下的 CLBU=0x90000000 只是
         * 它把 DMW VA 直接写进去的残留，核心读命令列表用低 32 位即可
         * 命中 DDR；数据突发必须用可解码的 32 位 PA，否则 HBFS。 */
        sata_writel(&PORT32(PORT_LST_ADDR), (uint32)(clb_dma & 0xffffffffULL));
        sata_writel(&PORT32(PORT_LST_ADDR_HI), (uint32)(clb_dma >> 32));
        sata_writel(&PORT32(PORT_FIS_ADDR), (uint32)(fb_dma & 0xffffffffULL));
        sata_writel(&PORT32(PORT_FIS_ADDR_HI), 0);
    }

    /* 手册（32 章）P0_DMACR 在端口偏移 0x170（HBA 基址 +0x100 +0x70）。
     * 官方 U-Boot ahci 和 Linux 标准 ahci 驱动都不写它也能正常传数据，
     * 这里只读不打乱默认值。 */
    printf("[sata] DMACR(0x170)=0x%x\n", PORT32(0x70));

    sata_writel(&PORT32(PORT_CMD), PORT_CMD_ICC_ACTIVE | PORT_CMD_FIS_RX |
                PORT_CMD_POWER_ON | PORT_CMD_SPIN_UP | PORT_CMD_START);

    printf("[sata] dbg CLB=%x%x FB=%x%x\n",
           PORT32(PORT_LST_ADDR_HI), PORT32(PORT_LST_ADDR),
           PORT32(PORT_FIS_ADDR_HI), PORT32(PORT_FIS_ADDR));
    if (sata_wait_spinup() != 0)
        return -1;
    return 0;
}

/* 尝试接管 U-Boot 已经初始化好的端口：HBA 已处于 AHCI 模式且端口在运行、
 * PHY 就绪时直接复用链路（跳过 HBA 复位和重协商）。这里只做检测与记录，
 * 不碰 PxCLB/PxFB、不停引擎；是否复用 U-Boot 环境由 sata_uboot_identify()
 * 决定，失败才回退到 sata_port_start() 换成内核自己的 DMA 结构。 */
static int sata_adopt_running_port(void)
{
    uint32 port_map, cmd, port_status;
    int i;

    if (!(HBA32(HOST_CTL) & HOST_AHCI_EN))
        return -1;

    port_map = HBA32(HOST_PORTS_IMPL);
    for (i = 0; i < 32; i++)
    {
        if (!(port_map & (1U << i)))
            continue;
        sata_port = i;
        port_status = PORT32(PORT_SCR_STAT);
        if ((port_status & PORT_SCR_STAT_DET_MASK) !=
            PORT_SCR_STAT_DET_PHYRDY)
            continue;
        cmd = PORT32(PORT_CMD);
        if (!(cmd & (PORT_CMD_START | PORT_CMD_FIS_RX)))
            continue;

        printf("[sata] adopt running port %d (SSTS=0x%x)\n",
               i, port_status);
        printf("[sata] uboot CLB=%x%x FB=%x%x PxCMD=%x(ST=%d FRE=%d "
               "CR=%d FR=%d) CI=%x TFD=%x\n",
               PORT32(PORT_LST_ADDR_HI), PORT32(PORT_LST_ADDR),
               PORT32(PORT_FIS_ADDR_HI), PORT32(PORT_FIS_ADDR),
               cmd, cmd & 1, (cmd >> 4) & 1, (cmd >> 15) & 1,
               (cmd >> 14) & 1, PORT32(PORT_CMD_ISSUE),
               PORT32(PORT_TFDATA));
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* 命令提交（对应 U-Boot ahci_fill_sg + ahci_fill_cmd_slot +           */
/* ahci_device_data_io）                                               */
/* ------------------------------------------------------------------ */

static int sata_fill_sg(struct ahci_cmd_tbl *tbl, uchar *buf, int buf_len)
{
    struct ahci_sg *sg = &tbl->sg[0];
    uint64 pa = sata_dma_addr((uint64)buf);   /* 0x9000 窗口 DMA 地址 */
    int sg_count = ((buf_len - 1) / MAX_DATA_BYTE_COUNT) + 1;
    int i;

    if (sg_count > AHCI_MAX_SG)
        return -1;

    for (i = 0; i < sg_count; i++)
    {
        /* 32 位物理地址（手册 6.4 节）：高 32 位写 0。U-Boot scsi read
         * 用 virt_to_phys 后的 32 位 PA 成功读盘；40 位 0x9000 前缀地址
         * 在数据突发时不被内存控制器解码，产生 HBFS（PxIS bit27）。 */
        sg[i].addr = (uint32)(pa & 0xffffffffULL);
        sg[i].addr_hi = (uint32)(pa >> 32);
        sg[i].flags_size = 0x3fffffU &
            (buf_len < MAX_DATA_BYTE_COUNT ?
             (uint32)(buf_len - 1) :
             (uint32)(MAX_DATA_BYTE_COUNT - 1));
        buf_len -= MAX_DATA_BYTE_COUNT;
        pa += MAX_DATA_BYTE_COUNT;
    }
    return sg_count;
}

/*
 * 提交一条命令并等待完成（slot 0）。
 *   fis       H2D Register FIS（20 字节）
 *   buf       数据缓冲（任意 DMW 窗口 VA；sata_dma_addr 会转成 32 位 PA）
 *   buf_len   数据字节数
 *   is_write  1 = 写盘（H2D），0 = 读盘
 */
static int sata_io(uint8 *fis, int fis_len, uchar *buf, int buf_len,
                   int is_write)
{
    uint64 clb_va = sata_uboot_mode ? sata_uboot_clb : (uint64)sata_cmdlist;
    uint64 ct_va  = sata_uboot_mode ? sata_uboot_ct  : (uint64)sata_cmdtbl;
    struct ahci_cmd_hdr *hdr =
        (struct ahci_cmd_hdr *)(uintptr_t)sata_uncached_view(clb_va);
    struct ahci_cmd_tbl *tbl =
        (struct ahci_cmd_tbl *)(uintptr_t)sata_uncached_view(ct_va);
    uint64 ct_dma = sata_dma_addr(ct_va);
    uint32 port_status, opts;
    uint64 deadline, t0;
    int sg_count;

    port_status = PORT32(PORT_SCR_STAT);
    if ((port_status & PORT_SCR_STAT_DET_MASK) !=
        PORT_SCR_STAT_DET_PHYRDY)
        return -1;

    memset(tbl, 0, sizeof(*tbl));
    memmove(tbl->cfis, fis, fis_len);

    sg_count = sata_fill_sg(tbl, buf, buf_len);
    if (sg_count < 0)
        return -1;

    /* opts：cfl(命令 FIS 长度，dword) | prdtl<<16 | W(写)<<6
     * 与 U-Boot ahci_device_data_io 完全一致 */
    opts = (uint32)(fis_len >> 2) | ((uint32)sg_count << 16) |
           (is_write ? (1U << 6) : 0);
    hdr->opts = opts;
    hdr->status = 0;
    hdr->tbl_addr = (uint32)(ct_dma & 0xffffffffULL);
    hdr->tbl_addr_hi = (uint32)(ct_dma >> 32);

    /* 缓存写回：HBA 经 DMA 读命令头/命令表/SG；写盘数据也要先落盘 */
    sata_cache_clean(clb_va, AHCI_CMD_SLOT_SZ);
    sata_cache_clean(ct_va, AHCI_CMD_TBL_HDR + 16);
    if (is_write)
        sata_cache_clean((uint64)buf, buf_len);
    /* 实验：清除 DMA 结构/数据缓冲在 L2 里的陈旧行 */
    sata_flush_cached(clb_va, AHCI_CMD_SLOT_SZ);
    sata_flush_cached(ct_va, AHCI_CMD_TBL_HDR + 16);
    if (is_write)
        sata_flush_cached((uint64)buf, buf_len);
    __sync_synchronize();

    /* 清残留错误/中断状态后下发（U-Boot writel_with_flush 语义） */
    PORT32(PORT_SCR_ERR) = PORT32(PORT_SCR_ERR);
    PORT32(PORT_IRQ_STAT) = PORT32(PORT_IRQ_STAT);
    sata_writel(&PORT32(PORT_CMD_ISSUE), 1U << SATA_SLOT);

    /* 忙等命令完成（PxCI 槽位清 0），对应 U-Boot waiting_for_cmd_completed。
     * sata_trace_link 置位时（IDENTIFY 首次下发），每 1ms 采样一次端口/链路
     * 寄存器，状态变化即打印，用于定位 DIAG.X 出现的时机与链路行为。 */
    t0 = r_time();
    deadline = t0 + timer_freq * (SATA_WAIT_DATAIO_MS / 1000ULL);
    {
        int fast = 0;
        while (r_time() < deadline)
        {
            uint32 ci = PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT);
            if (sata_trace_link)
            {
                static uint32 l_tfd, l_is, l_ssts, l_serr, l_cmd;
                uint32 tfd = PORT32(PORT_TFDATA);
                uint32 is  = PORT32(PORT_IRQ_STAT);
                uint32 ssts = PORT32(PORT_SCR_STAT);
                uint32 serr = PORT32(PORT_SCR_ERR);
                uint32 cmd  = PORT32(PORT_CMD);
                uint64 ms = (r_time() - t0) / (timer_freq / 1000ULL);
                if (tfd != l_tfd || is != l_is || ssts != l_ssts ||
                    serr != l_serr || cmd != l_cmd || !ci)
                {
                    printf("[sata] t=%lldms CI=%x TFD=%x IS=%x "
                           "SSTS=%x(DET=%d SPD=%d IPM=%d) SERR=%x "
                           "PxCMD=%x(ST=%d FRE=%d CR=%d FR=%d)\n",
                           (unsigned long long)ms, ci, tfd, is,
                           ssts, ssts & 0x3, (ssts >> 4) & 0x7,
                           (ssts >> 8) & 0xf, serr, cmd,
                           cmd & 1, (cmd >> 4) & 1, (cmd >> 15) & 1,
                           (cmd >> 14) & 1);
                    l_tfd = tfd; l_is = is; l_ssts = ssts; l_serr = serr;
                    l_cmd = cmd;
                }
            }
            if (ci == 0)
                break;
            sata_udelay(fast++ < 20 ? 100 : 1000);
        }
    }
    if (PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT))
    {
        printf("[sata] io timeout: cmd=0x%x (CI=0x%x TFD=0x%x IS=0x%x "
               "SSTS=0x%x SERR=0x%x hdr_status=0x%x)\n",
               fis[2], PORT32(PORT_CMD_ISSUE), PORT32(PORT_TFDATA),
               PORT32(PORT_IRQ_STAT), PORT32(PORT_SCR_STAT),
               PORT32(PORT_SCR_ERR), hdr->status);
        return -1;
    }
    if (PORT32(PORT_IRQ_STAT) & PORT_IRQ_TF_ERR)
    {
        printf("[sata] task file error: cmd=0x%x (TFD=0x%x)\n",
               fis[2], PORT32(PORT_TFDATA));
        return -1;
    }
    if (PORT32(PORT_TFDATA) & 0x1U) /* ERR 位 */
    {
        printf("[sata] tfd error: cmd=0x%x (TFD=0x%x)\n",
               fis[2], PORT32(PORT_TFDATA));
        return -1;
    }
    /* 读命令：设备经 DMA 写回数据，CPU 读取前先失效缓存行 */
    if (!is_write)
        sata_cache_invalidate((uint64)buf, buf_len);
    return 0;
}

/* 端口级错误恢复：清错误状态，必要时 COMRESET 重协商链路。
 * 对应 Linux ata_eh 的 PHY 复位流程。 */
static int sata_port_recover(void)
{
    uint32 tmp;
    uint64 j;

    PORT32(PORT_SCR_ERR) = PORT32(PORT_SCR_ERR);
    PORT32(PORT_IRQ_STAT) = PORT32(PORT_IRQ_STAT);

    /* 命令槽还卡着才需要复位端口；否则直接重发即可 */
    if (!(PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT)))
        return 0;

    printf("[sata] port reset (COMRESET)\n");
    tmp = PORT32(PORT_CMD);
    tmp &= ~(PORT_CMD_START | PORT_CMD_FIS_RX);
    sata_writel(&PORT32(PORT_CMD), tmp);
    sata_udelay(1000);

    tmp = PORT32(PORT_SCR_CTL);
    tmp = (tmp & ~0xfU) | 1U;   /* DET=1：链路复位 */
    sata_writel(&PORT32(PORT_SCR_CTL), tmp);
    sata_udelay(1000);
    tmp = (tmp & ~0xfU) | 0U;   /* DET=0：重新协商 */
    sata_writel(&PORT32(PORT_SCR_CTL), tmp);
    sata_udelay(1000);

    if (sata_link_up() != 0)
    {
        printf("[sata] link up failed after COMRESET\n");
        return -1;
    }
    for (j = 0; j < SATA_WAIT_SPINUP_MS; j++)
    {
        tmp = PORT32(PORT_TFDATA);
        if (!(tmp & ATA_BUSY))
            break;
        sata_udelay(1000);
    }

    PORT32(PORT_SCR_ERR) = PORT32(PORT_SCR_ERR);
    PORT32(PORT_IRQ_STAT) = PORT32(PORT_IRQ_STAT);
    sata_writel(&PORT32(PORT_CMD), PORT_CMD_ICC_ACTIVE | PORT_CMD_FIS_RX |
                PORT_CMD_POWER_ON | PORT_CMD_SPIN_UP | PORT_CMD_START);
    if (sata_wait_spinup() != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* IDENTIFY DEVICE：读磁盘容量等信息，失败时按标准流程恢复并重试        */
/* ------------------------------------------------------------------ */

#if 0
/* 已废弃的 U-Boot 环境复用实验（板测结论：用 U-Boot 原封不动的
 * CLB/FB/命令表也复现 DIAG.X，问题定位到内核侧缓存/地址约定）。
 * 现采用非缓存窗口 + U-Boot 64 位 DMA 地址约定，本实验保留待删除。 */
/* 决定性实验：完全复用 U-Boot 留下的 DMA 环境。
 * 不写 PxCLB/PxFB、不停止端口引擎；命令头写回 U-Boot 的 slot0，
 * 命令表写回 U-Boot 的 cmd_tbl，数据缓冲沿用 U-Boot 遗留 SG 指向的
 * 地址，SG 高32位沿用 U-Boot 的约定。等于把 U-Boot 探测时成功执行过的
 * IDENTIFY 原样再跑一遍，用于区分“引擎重启破坏了数据路径”和
 * “我们自己的结构/地址约定有问题”。 */
static int sata_uboot_identify(void)
{
    uint32 clb_lo = PORT32(PORT_LST_ADDR), clb_hi = PORT32(PORT_LST_ADDR_HI);
    uint32 fb_lo  = PORT32(PORT_FIS_ADDR), fb_hi  = PORT32(PORT_FIS_ADDR_HI);
    uint64 clb = ((uint64)clb_hi << 32) | clb_lo;
    uint64 fb  = ((uint64)fb_hi  << 32) | fb_lo;
    volatile uint32 *slot0;
    volatile uint32 *rfis;
    struct ahci_cmd_hdr *hdr;
    struct ahci_cmd_tbl *tbl;
    uint8 fis[20];
    uint64 ct, data_addr;
    uint32 sg_hi, sg_lo;
    uint64 deadline, t0;
    int i, ok = -1;

    printf("[sata] == UBOOT-ENV EXPERIMENT ==\n");
    if ((clb & ~0xffffffffULL) != 0x9000000000000000ULL ||
        clb == 0)
    {
        printf("[sata] uboot CLB=%x%x not in 0x9000 window, skip\n",
               clb_hi, clb_lo);
        return -1;
    }
    if (PORT32(PORT_CMD_ISSUE) != 0)
    {
        printf("[sata] engine busy CI=0x%x, skip\n",
               PORT32(PORT_CMD_ISSUE));
        return -1;
    }
    if (PORT32(PORT_TFDATA) & ATA_BUSY)
    {
        printf("[sata] device busy TFD=0x%x, skip\n",
               PORT32(PORT_TFDATA));
        return -1;
    }

    /* 1. 读 U-Boot 遗留的 slot0 命令头与命令表（真机基准，不猜） */
    slot0 = (volatile uint32 *)(uintptr_t)clb;
    sata_cache_invalidate(clb, AHCI_CMD_SLOT_SZ);
    printf("[sata] uboot slot0: opts=%x status=%x ct=%x%x "
           "rsvd=%x %x %x %x\n",
           slot0[0], slot0[1], slot0[3], slot0[2],
           slot0[4], slot0[5], slot0[6], slot0[7]);
    ct = ((uint64)slot0[3] << 32) | slot0[2];
    if (ct == 0)
    {
        printf("[sata] uboot slot0 ctba=0, skip\n");
        return -1;
    }

    tbl = (struct ahci_cmd_tbl *)(uintptr_t)ct;
    sata_cache_invalidate(ct, AHCI_CMD_TBL_HDR + 16);
    sg_hi = tbl->sg[0].addr_hi;
    sg_lo = tbl->sg[0].addr;
    printf("[sata] uboot cmdtbl=%x%x cfis=%x %x %x %x %x "
           "sg=%x%x sz=%x\n",
           (uint32)(ct >> 32), (uint32)ct,
           tbl->cfis[0], tbl->cfis[1], tbl->cfis[2],
           tbl->cfis[3], tbl->cfis[4],
           sg_hi, sg_lo, tbl->sg[0].flags_size);
    data_addr = ((uint64)sg_hi << 32) | sg_lo;
    if (data_addr == 0)
    {
        printf("[sata] uboot sg addr=0, skip\n");
        return -1;
    }

    /* 2. 记录 U-Boot 约定（整个 I/O 若走 U-Boot 环境都要沿用） */
    sata_uboot_clb = clb;
    sata_uboot_ct  = ct;
    sata_uboot_sg_hi = sg_hi;

    /* 3. 预填 0x55：完成后仍是 0x55 说明 HBA 没写数据 */
    memset((void *)(uintptr_t)data_addr, 0x55, 512);
    sata_cache_clean(data_addr, 512);

    /* 4. 写回 U-Boot slot0（ctba 保持不变），命令表写回 U-Boot cmd_tbl */
    memset((void *)(uintptr_t)clb, 0, AHCI_CMD_SLOT_SZ);
    hdr = (struct ahci_cmd_hdr *)(uintptr_t)clb;
    hdr->opts = 0x10005;              /* cfl=5, prdtl=1 */
    hdr->status = 0;
    hdr->tbl_addr = (uint32)ct;
    hdr->tbl_addr_hi = (uint32)(ct >> 32);

    memset(fis, 0, sizeof(fis));
    fis[0] = 0x27;
    fis[1] = 1 << 7;
    fis[2] = ATA_CMD_ID_ATA;
    memset((void *)(uintptr_t)ct, 0, AHCI_CMD_TBL_HDR + 16);
    memmove(tbl->cfis, fis, sizeof(fis));
    tbl->sg[0].addr = (uint32)data_addr;
    tbl->sg[0].addr_hi = sg_hi;
    tbl->sg[0].flags_size = ATA_SECT_SIZE - 1;

    sata_cache_clean(clb, AHCI_CMD_SLOT_SZ);
    sata_cache_clean(ct, AHCI_CMD_TBL_HDR + 16);
    __sync_synchronize();

    /* 4b. 非缓存窗口回读：确认 HBA 经 DMA 读到的（DRAM 里的）命令头和
     *     命令表/SG 就是我们写的内容。若缓存视图一致、非缓存视图是旧的，
     *     说明 CPU 写入没落到 DRAM（L2 一致性问题）。 */
    {
        volatile uint32 *uc_clb = sata_uncached_view(clb);
        volatile uint32 *uc_ct  = sata_uncached_view(ct);
        printf("[sata] uboot UNCACHED slot0: opts=%x status=%x ct=%x%x\n",
               uc_clb[0], uc_clb[1], uc_clb[3], uc_clb[2]);
        printf("[sata] uboot UNCACHED cmdtbl: cfis=%x %x %x %x %x "
               "sg=%x%x %x\n",
               uc_ct[0], uc_ct[1], uc_ct[2], uc_ct[3], uc_ct[4],
               uc_ct[0x20], uc_ct[0x21], uc_ct[0x22]);
    }

    /* 5. 下发前先看 U-Boot FIS 区遗留的 D2H（它最后一次成功命令） */
    sata_cache_invalidate(fb, AHCI_RX_FIS_SZ);
    rfis = (volatile uint32 *)(uintptr_t)(fb + 0x40);
    printf("[sata] uboot old D2H fis@%x%x+40:", (uint32)(fb >> 32),
           (uint32)fb);
    for (i = 0; i < 8; i++)
        printf(" %x", rfis[i]);
    printf("\n");

    PORT32(PORT_SCR_ERR) = PORT32(PORT_SCR_ERR);
    PORT32(PORT_IRQ_STAT) = PORT32(PORT_IRQ_STAT);
    sata_writel(&PORT32(PORT_CMD_ISSUE), 1U << SATA_SLOT);

    /* 6. 轮询完成：前 2ms 每 100us 采样一次（抓 DIAG.X 出现的精确时机），
     *    之后每 1ms；只打印状态变化。 */
    t0 = r_time();
    deadline = t0 + timer_freq * (SATA_WAIT_DATAIO_MS / 1000ULL);
    {
        uint32 l_tfd = PORT32(PORT_TFDATA), l_is = PORT32(PORT_IRQ_STAT);
        uint32 l_ssts = PORT32(PORT_SCR_STAT), l_serr = PORT32(PORT_SCR_ERR);
        uint32 l_cmd = PORT32(PORT_CMD);
        int fast = 0;

        printf("[sata] uboot t=0us CI=%x TFD=%x IS=%x SSTS=%x "
               "SERR=%x PxCMD=%x\n",
               PORT32(PORT_CMD_ISSUE) & 1, l_tfd, l_is, l_ssts,
               l_serr, l_cmd);
        while (r_time() < deadline)
        {
            uint32 ci = PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT);
            uint32 tfd = PORT32(PORT_TFDATA);
            uint32 is  = PORT32(PORT_IRQ_STAT);
            uint32 ssts = PORT32(PORT_SCR_STAT);
            uint32 serr = PORT32(PORT_SCR_ERR);
            uint32 cmd = PORT32(PORT_CMD);
            uint64 us = (r_time() - t0) / (timer_freq / 1000000ULL);

            if (tfd != l_tfd || is != l_is || ssts != l_ssts ||
                serr != l_serr || cmd != l_cmd || !ci)
            {
                printf("[sata] uboot t=%lluus CI=%x TFD=%x IS=%x "
                       "SSTS=%x(DET=%d SPD=%d IPM=%d) SERR=%x "
                       "PxCMD=%x(ST=%d FRE=%d CR=%d FR=%d)\n",
                       (unsigned long long)us, ci, tfd, is,
                       ssts, ssts & 0x3, (ssts >> 4) & 0x7,
                       (ssts >> 8) & 0xf, serr, cmd,
                       cmd & 1, (cmd >> 4) & 1, (cmd >> 15) & 1,
                       (cmd >> 14) & 1);
                l_tfd = tfd; l_is = is; l_ssts = ssts;
                l_serr = serr; l_cmd = cmd;
            }
            if (ci == 0)
                break;
            sata_udelay(fast++ < 20 ? 100 : 1000);
        }
    }

    /* 7. 结果判断与诊断 */
    if (PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT))
    {
        printf("[sata] uboot identify TIMEOUT (CI=0x%x TFD=0x%x "
               "IS=0x%x SERR=0x%x hdr_status=0x%x)\n",
               PORT32(PORT_CMD_ISSUE), PORT32(PORT_TFDATA),
               PORT32(PORT_IRQ_STAT), PORT32(PORT_SCR_ERR),
               hdr->status);
        goto diag;
    }
    if (PORT32(PORT_IRQ_STAT) & PORT_IRQ_TF_ERR)
    {
        printf("[sata] uboot identify TFERR (TFD=0x%x)\n",
               PORT32(PORT_TFDATA));
        goto diag;
    }
    if (PORT32(PORT_TFDATA) & 0x1U)
    {
        printf("[sata] uboot identify TFD-ERR (TFD=0x%x)\n",
               PORT32(PORT_TFDATA));
        goto diag;
    }

    sata_cache_invalidate(data_addr, 512);
    {
        volatile uint32 *cached_id =
            (volatile uint32 *)(uintptr_t)data_addr;
        volatile uint32 *uncached_id = sata_uncached_view(data_addr);
        uint64 lba48 = (uint64)id[100] | ((uint64)id[101] << 16) |
                       ((uint64)id[102] << 32) | ((uint64)id[103] << 48);
        volatile uint16 *id;
        uint32 lba28;

        if (cached_id[0] == 0x55555555U && uncached_id[0] == 0x55555555U)
        {
            printf("[sata] uboot identify done but data still 0x55 "
                   "(cached+uncached, HBA 未写数据)\n");
            goto diag;
        }
        if (cached_id[0] == 0x55555555U && uncached_id[0] != 0x55555555U)
            printf("[sata] uboot identify: CACHED view stale 0x55, "
                   "UNCACHED=%x %x %x %x (L2 掩盖!)\n",
                   uncached_id[0], uncached_id[1], uncached_id[2],
                   uncached_id[3]);

        id = (volatile uint16 *)uncached_id;
        lba48 = (uint64)id[100] | ((uint64)id[101] << 16) |
                ((uint64)id[102] << 32) | ((uint64)id[103] << 48);
        lba28 = (uint32)id[60] | ((uint32)id[61] << 16);

        sata_nsectors = lba48 ? lba48 : lba28;
        printf("[sata] uboot identify OK data=%x %x %x %x "
               "sectors=%d\n",
               uncached_id[0], uncached_id[1], uncached_id[2],
               uncached_id[3],
               (uint32)sata_nsectors);
    }
    return 0;

diag:
    ok = -1;
    {
        uint32 serr = PORT32(PORT_SCR_ERR);
        volatile uint32 *pio = (volatile uint32 *)(uintptr_t)(fb + 0x20);
        volatile uint32 *id2 = (volatile uint32 *)(uintptr_t)data_addr;

        sata_cache_invalidate(fb, AHCI_RX_FIS_SZ);
        sata_cache_invalidate(data_addr, 512);
        printf("[sata] uboot dbg SERR=0x%x (%s%s%s%s%s%s%s) "
               "SSTS=0x%x SCTL=0x%x SIG=0x%x\n",
               serr,
               serr & 0x1 ? "D" : "", serr & 0x2 ? "P" : "",
               serr & 0x4 ? "C" : "", serr & 0x8 ? "T" : "",
               serr & 0x10 ? "M" : "", serr & 0x100 ? "X" : "",
               serr & 0x8000 ? "F" : "",
               PORT32(PORT_SCR_STAT), PORT32(PORT_SCR_CTL),
               PORT32(PORT_SIG));
        printf("[sata] uboot dbg PIO Setup fis@%x%x+20:",
               (uint32)(fb >> 32), (uint32)fb);
        for (i = 0; i < 6; i++)
            printf(" %x", pio[i]);
        printf("\n");
        printf("[sata] uboot dbg D2H fis@%x%x+40:",
               (uint32)(fb >> 32), (uint32)fb);
        for (i = 0; i < 8; i++)
            printf(" %x", rfis[i]);
        printf("\n");
        printf("[sata] uboot dbg data@%x%x: %x %x %x %x %x %x %x %x\n",
               (uint32)(data_addr >> 32), (uint32)data_addr,
               id2[0], id2[1], id2[2], id2[3],
               id2[4], id2[5], id2[6], id2[7]);
        printf("[sata] uboot dbg hdr=%x %x %x%x sg=%x%x %x\n",
               hdr->opts, hdr->status,
               hdr->tbl_addr_hi, hdr->tbl_addr,
               tbl->sg[0].addr_hi, tbl->sg[0].addr,
               tbl->sg[0].flags_size);
        /* 非缓存窗口回读：缓存视图可能被 L2 掩盖，直接看 DRAM */
        {
            volatile uint32 *uc_pio  = sata_uncached_view(fb + 0x20);
            volatile uint32 *uc_d2h  = sata_uncached_view(fb + 0x40);
            volatile uint32 *uc_data = sata_uncached_view(data_addr);
            volatile uint32 *uc_clb  = sata_uncached_view(clb);
            volatile uint32 *uc_ct   = sata_uncached_view(ct);

            printf("[sata] uboot UNCACHED PIO:");
            for (i = 0; i < 6; i++)
                printf(" %x", uc_pio[i]);
            printf("\n");
            printf("[sata] uboot UNCACHED D2H:");
            for (i = 0; i < 8; i++)
                printf(" %x", uc_d2h[i]);
            printf("\n");
            printf("[sata] uboot UNCACHED data: %x %x %x %x %x %x %x %x\n",
                   uc_data[0], uc_data[1], uc_data[2], uc_data[3],
                   uc_data[4], uc_data[5], uc_data[6], uc_data[7]);
            printf("[sata] uboot UNCACHED hdr=%x %x ct=%x%x "
                   "sg=%x%x %x\n",
                   uc_clb[0], uc_clb[1], uc_clb[3], uc_clb[2],
                   uc_ct[0x20], uc_ct[0x21], uc_ct[0x22]);
        }
    }
    return ok;
}
#endif

/* 主路径：完全复用 U-Boot 连续运行的端口引擎。
 * 不写 PxCLB/PxFB、不停止引擎；命令头/命令表经非缓存窗口写入 U-Boot
 * 的 slot0/cmd_tbl（保证 CPU 写入即刻落在 DRAM，HBA 一定读得到），
 * 数据缓冲用我们自己的非缓存 bank1 缓冲，SG 用 U-Boot 实测的 64 位
 * 地址格式（高32位 0x90000000）。成功则后续所有 I/O 都走该引擎。 */
static int sata_uboot_mode_identify(void)
{
    uint32 clb_lo = PORT32(PORT_LST_ADDR), clb_hi = PORT32(PORT_LST_ADDR_HI);
    uint32 fb_lo  = PORT32(PORT_FIS_ADDR), fb_hi  = PORT32(PORT_FIS_ADDR_HI);
    uint64 clb = ((uint64)clb_hi << 32) | clb_lo;
    uint64 fb  = ((uint64)fb_hi  << 32) | fb_lo;
    volatile uint32 *uc_clb;
    struct ahci_cmd_hdr *hdr;
    struct ahci_cmd_tbl *tbl;
    uint8 fis[20];
    uint64 ct;
    uint64 deadline, t0;
    int i, fast = 0;

    printf("[sata] == UBOOT-ENGINE (continuous) ==\n");
    if (clb == 0 || (clb & ~0xffffffffULL) == 0)
    {
        printf("[sata] uboot CLB=%x%x invalid, skip\n", clb_hi, clb_lo);
        return -1;
    }
    if (PORT32(PORT_CMD_ISSUE) != 0)
    {
        printf("[sata] engine busy CI=0x%x, skip\n",
               PORT32(PORT_CMD_ISSUE));
        return -1;
    }

    /* 1. 从 U-Boot slot0 读出命令表地址（非缓存窗口读，保证是 DRAM 内容） */
    uc_clb = sata_uncached_view(clb);
    ct = ((uint64)uc_clb[3] << 32) | uc_clb[2];
    printf("[sata] uboot CLB=%x%x FB=%x%x slot0 opts=%x status=%x "
           "ct=%x%x\n",
           clb_hi, clb_lo, fb_hi, fb_lo,
           uc_clb[0], uc_clb[1], (uint32)(ct >> 32), (uint32)ct);
    if (ct == 0)
    {
        printf("[sata] uboot slot0 ctba=0, skip\n");
        return -1;
    }

    sata_uboot_clb = clb;
    sata_uboot_ct  = ct;

    /* 2. 预填数据缓冲 0x55（非缓存，直写 DRAM） */
    memset(sata_ident, 0x55, 512);

    /* 3. 先清掉 U-Boot 命令区在 L2 的陈旧行，再用缓存别名写入
     *    （与 U-Boot 自身写法一致：缓存写会更新 L2，HBA 的 DMA 读
     *    经 L2 探查能拿到最新内容；之前用非缓存写覆盖 DRAM，但 L2
     *    里 U-Boot 的旧命令行没动，HBA 可能读到旧命令）。 */
    sata_flush_cached(clb, AHCI_CMD_SLOT_SZ * AHCI_MAX_CMD_SLOT);
    sata_flush_cached(ct, AHCI_CMD_TBL_HDR + 16);
    hdr = (struct ahci_cmd_hdr *)(uintptr_t)clb;
    memset((void *)(uintptr_t)clb, 0, AHCI_CMD_SLOT_SZ);
    hdr->opts = 0x10005;
    hdr->status = 0;
    hdr->tbl_addr = (uint32)(sata_dma_addr(ct) & 0xffffffffULL);
    hdr->tbl_addr_hi = (uint32)(sata_dma_addr(ct) >> 32);

    memset(fis, 0, sizeof(fis));
    fis[0] = 0x27;
    fis[1] = 1 << 7;
    fis[2] = ATA_CMD_ID_ATA;
    tbl = (struct ahci_cmd_tbl *)(uintptr_t)ct;
    memset((void *)(uintptr_t)ct, 0, AHCI_CMD_TBL_HDR + 16);
    memmove(tbl->cfis, fis, sizeof(fis));
    tbl->sg[0].addr = (uint32)(sata_dma_addr((uint64)sata_ident) &
                               0xffffffffULL);
    tbl->sg[0].addr_hi = (uint32)(sata_dma_addr((uint64)sata_ident) >> 32);
    tbl->sg[0].flags_size = ATA_SECT_SIZE - 1;

    /* 4. 下发（清残留错误/中断） */
    PORT32(PORT_SCR_ERR) = PORT32(PORT_SCR_ERR);
    PORT32(PORT_IRQ_STAT) = PORT32(PORT_IRQ_STAT);
    sata_writel(&PORT32(PORT_CMD_ISSUE), 1U << SATA_SLOT);

    /* 5. 轮询：前 2ms 每 100us 采样，之后每 1ms；只打印状态变化 */
    t0 = r_time();
    deadline = t0 + timer_freq * (SATA_WAIT_DATAIO_MS / 1000ULL);
    {
        uint32 l_tfd = PORT32(PORT_TFDATA), l_is = PORT32(PORT_IRQ_STAT);
        uint32 l_ssts = PORT32(PORT_SCR_STAT), l_serr = PORT32(PORT_SCR_ERR);
        uint32 l_cmd = PORT32(PORT_CMD);

        printf("[sata] uboot t=0us CI=%x TFD=%x IS=%x SSTS=%x "
               "SERR=%x PxCMD=%x\n",
               PORT32(PORT_CMD_ISSUE) & 1, l_tfd, l_is, l_ssts,
               l_serr, l_cmd);
        while (r_time() < deadline)
        {
            uint32 ci = PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT);
            uint32 tfd = PORT32(PORT_TFDATA);
            uint32 is  = PORT32(PORT_IRQ_STAT);
            uint32 ssts = PORT32(PORT_SCR_STAT);
            uint32 serr = PORT32(PORT_SCR_ERR);
            uint32 cmd = PORT32(PORT_CMD);
            uint64 us = (r_time() - t0) / (timer_freq / 1000000ULL);

            if (tfd != l_tfd || is != l_is || ssts != l_ssts ||
                serr != l_serr || cmd != l_cmd || !ci)
            {
                printf("[sata] uboot t=%lluus CI=%x TFD=%x IS=%x "
                       "SSTS=%x(DET=%d SPD=%d IPM=%d) SERR=%x "
                       "PxCMD=%x(ST=%d FRE=%d CR=%d FR=%d)\n",
                       (unsigned long long)us, ci, tfd, is,
                       ssts, ssts & 0x3, (ssts >> 4) & 0x7,
                       (ssts >> 8) & 0xf, serr, cmd,
                       cmd & 1, (cmd >> 4) & 1, (cmd >> 15) & 1,
                       (cmd >> 14) & 1);
                l_tfd = tfd; l_is = is; l_ssts = ssts;
                l_serr = serr; l_cmd = cmd;
            }
            if (ci == 0)
                break;
            sata_udelay(fast++ < 20 ? 100 : 1000);
        }
    }

    /* 6. 结果判断 */
    if (PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT))
    {
        printf("[sata] uboot identify TIMEOUT (CI=0x%x TFD=0x%x "
               "IS=0x%x SERR=0x%x hdr_status=0x%x)\n",
               PORT32(PORT_CMD_ISSUE), PORT32(PORT_TFDATA),
               PORT32(PORT_IRQ_STAT), PORT32(PORT_SCR_ERR),
               hdr->status);
        goto diag;
    }
    if ((PORT32(PORT_IRQ_STAT) & PORT_IRQ_TF_ERR) ||
        (PORT32(PORT_TFDATA) & 0x1U))
    {
        printf("[sata] uboot identify TFERR (TFD=0x%x)\n",
               PORT32(PORT_TFDATA));
        goto diag;
    }
    if (((volatile uint32 *)sata_ident)[0] == 0x55555555U)
    {
        printf("[sata] uboot identify done but data still 0x55\n");
        goto diag;
    }
    {
        volatile uint16 *id = (volatile uint16 *)sata_ident;
        uint64 lba48 = (uint64)id[100] | ((uint64)id[101] << 16) |
                       ((uint64)id[102] << 32) | ((uint64)id[103] << 48);
        uint32 lba28 = (uint32)id[60] | ((uint32)id[61] << 16);

        sata_nsectors = lba48 ? lba48 : lba28;
        printf("[sata] uboot identify OK data=%x %x %x %x sectors=%d\n",
               ((volatile uint32 *)sata_ident)[0],
               ((volatile uint32 *)sata_ident)[1],
               ((volatile uint32 *)sata_ident)[2],
               ((volatile uint32 *)sata_ident)[3],
               (uint32)sata_nsectors);
    }
    return 0;

diag:
    {
        uint32 serr = PORT32(PORT_SCR_ERR);
        volatile uint32 *pio = sata_uncached_view(fb + 0x20);
        volatile uint32 *d2h = sata_uncached_view(fb + 0x40);

        printf("[sata] uboot dbg SERR=0x%x (%s%s%s%s%s%s%s%s) "
               "SSTS=0x%x SIG=0x%x\n",
               serr,
               serr & 0x1 ? "D" : "", serr & 0x2 ? "P" : "",
               serr & 0x4 ? "C" : "", serr & 0x8 ? "T" : "",
               serr & 0x10 ? "M" : "", serr & 0x100 ? "T8" : "",
               serr & 0x200 ? "C9" : "", serr & 0x400 ? "P10" : "",
               PORT32(PORT_SCR_STAT), PORT32(PORT_SIG));
        printf("[sata] uboot dbg PIO@%x%x+20:", (uint32)(fb >> 32),
               (uint32)fb);
        for (i = 0; i < 6; i++)
            printf(" %x", pio[i]);
        printf("\n");
        printf("[sata] uboot dbg D2H@%x%x+40:", (uint32)(fb >> 32),
               (uint32)fb);
        for (i = 0; i < 8; i++)
            printf(" %x", d2h[i]);
        printf("\n");
        printf("[sata] uboot dbg data=%x %x %x %x hdr=%x %x "
               "ct=%x%x sg=%x%x %x\n",
               ((volatile uint32 *)sata_ident)[0],
               ((volatile uint32 *)sata_ident)[1],
               ((volatile uint32 *)sata_ident)[2],
               ((volatile uint32 *)sata_ident)[3],
               hdr->opts, hdr->status,
               hdr->tbl_addr_hi, hdr->tbl_addr,
               tbl->sg[0].addr_hi, tbl->sg[0].addr,
               tbl->sg[0].flags_size);
    }
    return -1;
}

static int sata_identify(void)
{
    uint8 fis[20];
    volatile uint16 *id = (volatile uint16 *)sata_ident;
    uint64 lba48;
    uint32 lba28;
    int attempt;

    memset(fis, 0, sizeof(fis));
    fis[0] = 0x27;         /* H2D FIS */
    fis[1] = 1 << 7;       /* C：Command FIS */
    fis[2] = ATA_CMD_ID_ATA;

    /* 实验：DMA READ（0x25）作为第一条命令，分别在全分页（PG=1）和
     * 关分页（PG=0，纯 DMW 直映射，与 U-Boot 一致）下各试一次，
     * 判断内核开分页是否影响 SATA 数据通路。 */
    for (attempt = 0; attempt < 2; attempt++)
    {
        uint64 crmd;
        __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
        printf("[sata] DMA READ first: PG=%d (CRMD=0x%llx)\n",
               (int)((crmd >> 3) & 1), (unsigned long long)crmd);
        if (attempt == 1)
            __asm__ volatile("csrwr %0, 0x0"
                             :: "r"(crmd & ~(1ULL << 3)));
        memset((void *)(uintptr_t)SATA_DMA_VA, 0x55, 0x2000);
        memset(fis, 0, sizeof(fis));
        fis[0] = 0x27;
        fis[1] = 1 << 7;
        fis[2] = ATA_CMD_READ_EXT;
        fis[7] = 1 << 6;
        fis[12] = 1;           /* 1 个扇区 */
        if (sata_io(fis, sizeof(fis), sata_ident, 512, 0) == 0)
        {
            printf("[sata] DMA READ PG%d OK data=%x %x %x %x\n",
                   (int)((crmd >> 3) & 1),
                   ((volatile uint32 *)sata_ident)[0],
                   ((volatile uint32 *)sata_ident)[1],
                   ((volatile uint32 *)sata_ident)[2],
                   ((volatile uint32 *)sata_ident)[3]);
        }
        else
        {
            printf("[sata] DMA READ PG%d FAILED (SERR=0x%x TFD=0x%x)\n",
                   (int)((crmd >> 3) & 1),
                   PORT32(PORT_SCR_ERR), PORT32(PORT_TFDATA));
        }
        if (attempt == 1)
            __asm__ volatile("csrwr %0, 0x0" :: "r"(crmd));
        if (sata_port_recover() != 0)
            return -1;
    }

    for (attempt = 0; attempt < SATA_IDENTIFY_RETRY; attempt++)
    {
        memset(fis, 0, sizeof(fis));
        fis[0] = 0x27;
        fis[1] = 1 << 7;
        fis[2] = ATA_CMD_ID_ATA;
        /* 预填整个 DMA 区 0x55：若完成后仍是 0x55，说明 HBA 根本没写数据；
         * 若数据落在区域别处，后面的 scan 会抓到 */
        memset((void *)(uintptr_t)SATA_DMA_VA, 0x55, 0x2000);
        sata_cache_clean((uint64)SATA_DMA_VA, 0x2000);

        sata_trace_link = (attempt == 0);
        if (sata_io(fis, sizeof(fis), sata_ident, 512, 0) == 0)
        {
            sata_trace_link = 0;
            break;
        }
        sata_trace_link = 0;

        printf("[sata] identify attempt %d failed\n", attempt + 1);
        if (attempt == 0)
        {
            struct ahci_cmd_hdr *hdr = (struct ahci_cmd_hdr *)sata_cmdlist;
            struct ahci_cmd_tbl *tbl = (struct ahci_cmd_tbl *)sata_cmdtbl;
            volatile uint32 *fis9 = (volatile uint32 *)((uint64)sata_fis + 0x20);
            volatile uint32 *id9 = (volatile uint32 *)sata_ident;
            volatile uint32 *fb0 = (volatile uint32 *)sata_fis;
            uint32 serr = PORT32(PORT_SCR_ERR);
            int i;

            /* 一次性诊断：命令槽/命令表/SG 在内存中的真实内容（HBA 的视角），
             * 以及 PIO Setup FIS 与 ident 缓冲。 */
            sata_cache_invalidate((uint64)sata_fis, AHCI_RX_FIS_SZ);
            sata_cache_invalidate((uint64)sata_ident, 512);
            sata_cache_invalidate((uint64)sata_cmdlist, AHCI_CMD_SLOT_SZ);
            sata_cache_invalidate((uint64)sata_cmdtbl, AHCI_CMD_TBL_HDR + 16);
            printf("[sata] dbg SERR=0x%x (%s%s%s%s%s%s%s) SCTL=0x%x SACT=0x%x SIG=0x%x\n",
                   serr,
                   serr & 0x1 ? "D" : "", serr & 0x2 ? "P" : "",
                   serr & 0x4 ? "C" : "", serr & 0x8 ? "T" : "",
                   serr & 0x10 ? "M" : "", serr & 0x100 ? "X" : "",
                   serr & 0x8000 ? "F" : "",
                   PORT32(PORT_SCR_CTL), PORT32(PORT_SACT),
                   PORT32(PORT_SIG));
            printf("[sata] dbg fis0=");
            for (i = 0; i < 24; i++)
                printf(" %x", fb0[i]);
            printf("\n");
            printf("[sata] dbg hdr=%x %x %x%x sg=%x%x %x\n",
                   hdr->opts, hdr->status,
                   hdr->tbl_addr_hi, hdr->tbl_addr,
                   tbl->sg[0].addr_hi, tbl->sg[0].addr,
                   tbl->sg[0].flags_size);
            printf("[sata] dbg pio=%x %x %x %x %x ident=%x %x %x %x\n",
                   fis9[0], fis9[1], fis9[2], fis9[3], fis9[4],
                   id9[0], id9[1], id9[2], id9[3]);
            /* 非缓存窗口回读：确认缓存视图是否被 L2 掩盖 */
            {
                volatile uint32 *uc_pio = sata_uncached_view(
                    (uint64)sata_fis + 0x20);
                volatile uint32 *uc_ident = sata_uncached_view(
                    (uint64)sata_ident);
                volatile uint32 *uc_clb = sata_uncached_view(
                    (uint64)sata_cmdlist);
                volatile uint32 *uc_ct  = sata_uncached_view(
                    (uint64)sata_cmdtbl);

                printf("[sata] dbg UNCACHED pio=%x %x %x %x %x "
                       "ident=%x %x %x %x\n",
                       uc_pio[0], uc_pio[1], uc_pio[2], uc_pio[3],
                       uc_pio[4],
                       uc_ident[0], uc_ident[1], uc_ident[2],
                       uc_ident[3]);
                printf("[sata] dbg UNCACHED hdr=%x %x ct=%x%x "
                       "sg=%x%x %x\n",
                       uc_clb[0], uc_clb[1], uc_clb[3], uc_clb[2],
                       uc_ct[0x20], uc_ct[0x21], uc_ct[0x22]);
                /* HBA 实际取到的命令 FIS（非缓存视图，DRAM 真实内容）：
                 * 应为 0x27 0x80 0xec 0x00 ...，若不一致说明命令被污染 */
                printf("[sata] dbg cfis(UNCACHED): %x %x %x %x %x "
                       "cmd=%x\n",
                       uc_ct[0], uc_ct[1], uc_ct[2], uc_ct[3], uc_ct[4],
                       (uint8)(uc_ct[2] >> 16));
                /* PIO Setup 传输计数（标准布局 byte11=Count[7:0]，
                 * byte12=Count[15:8]；IDENTIFY 应为 0x200） */
                printf("[sata] dbg PIO count byte11=%x byte12=%x\n",
                       (uint8)(uc_pio[2] >> 24),
                       (uint8)(uc_pio[3] & 0xff));
                /* D2H 解码 + 全套寄存器状态 */
                {
                    volatile uint32 *uc_d2h = sata_uncached_view(
                        (uint64)sata_fis + 0x40);
                    uint32 d2h0 = uc_d2h[0];

                    printf("[sata] dbg D2H(UNCACHED) type=%x status=%x "
                           "err=%x d0=%x\n",
                           (uint8)(d2h0 & 0xff),
                           (uint8)((d2h0 >> 16) & 0xff),
                           (uint8)((d2h0 >> 24) & 0xff), d2h0);
                }
                printf("[sata] dbg regs PxCMD=%x PxIE=%x PxSCTL=%x "
                       "GHC=%x CAP2=%x\n",
                       PORT32(PORT_CMD), PORT32(PORT_IRQ_MASK),
                       PORT32(PORT_SCR_CTL), HBA32(HOST_CTL),
                       HBA32(HOST_CAP2));
                /* 扫描 0x2000-0x3fff（dma+ident 预填 0x55 的区域），
                 * 找非 0x55/非 0 数据：若 HBA 把数据写到了别处，能发现 */
                {
                    volatile uint32 *uc_dma = sata_uncached_view(
                        (uint64)SATA_DMA_VA);
                    int hits = 0, j;

                    printf("[sata] dbg scan:");
                    for (j = 0; j < 0x2000 / 4; j++)
                    {
                        uint32 v = uc_dma[j];
                        if (v != 0x55555555U && v != 0)
                        {
                            printf(" +%x=%x", 0x2000 + j * 4, v);
                            if (++hits >= 8)
                                break;
                        }
                    }
                    printf("\n");
                }
            }

            /* 对照实验：IDENTIFY 是 PIO 命令；换发一个 DMA 读（READ EXT，
             * 扇区 0，1 块），验证控制器数据突发是否只在 PIO 下失效
             * （Loongson 机器上的已知问题：PIO 异常、DMA 正常）。 */
            memset(sata_ident, 0x55, 512);
            sata_cache_clean((uint64)sata_ident, 512);
            memset(fis, 0, sizeof(fis));
            fis[0] = 0x27;
            fis[1] = 1 << 7;
            fis[2] = ATA_CMD_READ_EXT;
            fis[7] = 1 << 6;
            fis[12] = 1;   /* 1 个扇区 */
            if (sata_io(fis, sizeof(fis), sata_ident, 512, 0) == 0)
            {
                volatile uint32 *id2 = (volatile uint32 *)sata_ident;
                sata_cache_invalidate((uint64)sata_ident, 512);
                printf("[sata] dbg DMA READ sector0 OK ident=%x %x %x %x\n",
                       id2[0], id2[1], id2[2], id2[3]);
            }
            else
            {
                printf("[sata] dbg DMA READ sector0 failed\n");
            }
        }
        if (sata_port_recover() != 0)
            return -1;
    }
    if (attempt == SATA_IDENTIFY_RETRY)
        return -1;

    if (PORT32(PORT_SIG) != 0x00000101U)
    {
        printf("[sata] unexpected signature 0x%x\n", PORT32(PORT_SIG));
        return -1;
    }

    lba48 = (uint64)id[100] | ((uint64)id[101] << 16) |
            ((uint64)id[102] << 32) | ((uint64)id[103] << 48);
    lba28 = (uint32)id[60] | ((uint32)id[61] << 16);
    sata_nsectors = lba48 ? lba48 : lba28;

    printf("[sata] disk identified: %d sectors (%d MB)\n",
           (uint32)sata_nsectors, (uint32)(sata_nsectors / 2048));
    return 0;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void sata_init(void)
{
    uint32 cmd;
    int dev, i;
    uint32 bar_phys = 0;
    int found = 0;

    initlock(&sata_lock, "sata");

    sata_ecam = (volatile uint32 *)(LA_DMW1_MASK | SATA_PCIE_ECAM_PHYS);
    sata_hba = NULL;
    sata_cfg_base = 0;

    /* 1. 找 Loongson SATA (0014:7a08)：设备树固定 device 8 fn 0，
     *    找不到再扫 0..31 兜底。 */
    {
        int try_dev[] = {LS2K_SATA_DEV, 0};
        for (i = 0; i < 2; i++)
        {
            if (try_dev[i] == 0 && found)
                break;
            dev = try_dev[i];
            if (dev == 0 && i == 1)
            {
                /* 全扫 */
                for (dev = 0; dev < 32; dev++)
                {
                    uint32 base = (uint32)((dev << 11) | (LS2K_SATA_FN << 8));
                    sata_cfg_base = base;
                    if (sata_cfg_read16(0) == LS2K_SATA_VENDOR &&
                        sata_cfg_read16(2) == LS2K_SATA_DEVICE)
                    {
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    break;
                continue;
            }
            sata_cfg_base = (uint32)((dev << 11) | (LS2K_SATA_FN << 8));
            if (sata_cfg_read16(0) == LS2K_SATA_VENDOR &&
                sata_cfg_read16(2) == LS2K_SATA_DEVICE)
            {
                found = 1;
                break;
            }
        }
    }
    if (!found)
    {
        printf("[sata] Loongson SATA controller not found on internal PCI bus\n");
        printf("[sata] check SATA_PCIE_ECAM_PHYS (default 0x1a000000)\n");
        printf("[sata] on vendor Linux: cat /proc/iomem | grep -i pci\n");
        return;
    }

    /* 2. AHCI ABAR：先按 U-Boot 取 BAR5，个别实现为空时回退扫描其他内存 BAR。 */
    for (i = 5; i >= 0; i--)
    {
        uint32 v = sata_cfg_read32(0x10 + 4 * i);
        if (v == 0 || (v & 0x1U))
            continue;
        bar_phys = v & ~0xfU;
        if (v & 0x4U)
        {
            uint32 hi = sata_cfg_read32(0x10 + 4 * (i + 1));
            bar_phys |= (hi & 0xfffffff0U);
        }
        break;
    }
    if (bar_phys == 0)
    {
        printf("[sata] no memory BAR\n");
        return;
    }

    /* 3. 使能 IO/内存/总线主控 */
    cmd = sata_cfg_read16(0x04);
    sata_cfg_write16(0x04, (uint16)(cmd | 0x7U));

    /* 4. 映射 HBA */
    sata_hba = (volatile uint32 *)(LA_DMW1_MASK | bar_phys);
    if (HBA32(HOST_CAP) == 0xffffffffU || HBA32(HOST_CAP) == 0)
    {
        printf("[sata] HBA invalid: CAP=0x%x (BAR=0x%x)\n",
               HBA32(HOST_CAP), bar_phys);
        sata_hba = NULL;
        return;
    }
    printf("[sata] HBA at 0x%x (CAP=0x%x)\n", bar_phys, HBA32(HOST_CAP));

    /* 实验：打印当前 CPU 核号，检测第二个核是否也在并发跑内核 */
    {
        uint64 cpuid;
        __asm__ volatile("csrrd %0, 0x20" : "=r"(cpuid));
        printf("[sata] running on cpu%lld\n", (unsigned long long)cpuid);
    }
    /* 实验：打印全套关键 CSR，与 U-Boot 运行状态对比 */
    {
        uint64 v;
        printf("[sata] csr CRMD=");
        __asm__ volatile("csrrd %0, 0x0" : "=r"(v));
        printf("%llx", (unsigned long long)v);
        printf(" DMW0=");
        __asm__ volatile("csrrd %0, 0x180" : "=r"(v));
        printf("%llx", (unsigned long long)v);
        printf(" DMW1=");
        __asm__ volatile("csrrd %0, 0x181" : "=r"(v));
        printf("%llx", (unsigned long long)v);
        printf(" EUEN=");
        __asm__ volatile("csrrd %0, 0x2" : "=r"(v));
        printf("%llx", (unsigned long long)v);
        printf(" ECFG=");
        __asm__ volatile("csrrd %0, 0x4" : "=r"(v));
        printf("%llx", (unsigned long long)v);
        printf(" TCFG=");
        __asm__ volatile("csrrd %0, 0x40" : "=r"(v));
        printf("%llx\n", (unsigned long long)v);
    }

    /* DWC SATA 核专用 OOBR（Out-of-Band Register，主机偏移 0xB8）：
     * 配置 COMINIT/COMWAKE 时序（cwMin=2,cwMAX=6,ciMin=0xb,ciMax=0x14）。
     * U-Boot dwc_ahsata 驱动在 host_init 里必须写它；通用 ahci.c 不写，
     * HBA 复位后可能被清回默认值，导致链路带外信号时序异常。 */
    HBA32(0xB8) = 0x80000000;      /* OOBR 写使能 */
    HBA32(0xB8) = 0x02060b14;
    printf("[sata] OOBR=0x%x\n", HBA32(0xB8));

    /* 5. 优先接管 U-Boot 已初始化好的端口（不触发 HBA 复位，保留启动
     *    代码配置的 PHY/DMA 状态）；无可用端口才做全量初始化。 */
    if (sata_adopt_running_port() != 0)
    {
        printf("[sata] no running port, full init\n");
        if (sata_host_init() != 0 || sata_port_start() != 0)
        {
            sata_hba = NULL;
            return;
        }
    }
    else if (sata_uboot_mode_identify() == 0)
    {
        sata_ready = 1;
        sata_uboot_mode = 1;
        printf("[sata] ready: port %d (UBOOT-ENGINE mode), %d sectors\n",
               sata_port, (uint32)sata_nsectors);
        return;
    }
    else
    {
        printf("[sata] uboot-engine failed, restart port with own "
               "structures\n");
        if (sata_port_start() != 0)
        {
            sata_hba = NULL;
            return;
        }
    }

    if (sata_identify() != 0)
    {
        sata_hba = NULL;
        return;
    }

    sata_ready = 1;
    printf("[sata] ready: port %d, %d sectors\n",
           sata_port, (uint32)sata_nsectors);
}

/*
 * 块设备读写入口，与 la_virtio_disk_rw 同签名，由 bio.c 调用。
 * b->blockno 是 BSIZE(4096) 块号，换算成 512 字节扇区。
 */
void la_sata_disk_rw(struct buf *b, int write)
{
    uint64 sector;
    uint8 fis[20];

    if (!sata_ready)
        panic("sata: disk not ready");

    acquire(&sata_lock);
    sector = (uint64)b->blockno * (BSIZE / 512);

    if (write)
        memmove(sata_dma, b->data, BSIZE);

    /* H2D Register FIS（与 U-Boot ata_scsiop_read_write 相同布局） */
    memset(fis, 0, sizeof(fis));
    fis[0] = 0x27;                          /* H2D FIS */
    fis[1] = 1 << 7;                        /* C：Command FIS */
    fis[2] = write ? ATA_CMD_WRITE_EXT : ATA_CMD_READ_EXT;
    fis[3] = 0xe0;                          /* features（U-Boot 原样） */
    fis[4] = (uint8)(sector);
    fis[5] = (uint8)(sector >> 8);
    fis[6] = (uint8)(sector >> 16);
    fis[7] = 1 << 6;                        /* device 寄存器：LBA 模式 */
    fis[8] = (uint8)(sector >> 24);
    fis[9] = (uint8)(sector >> 32);
    fis[10] = (uint8)(sector >> 40);
    fis[11] = 0;
    fis[12] = (uint8)(BSIZE / 512);         /* 扇区数 */
    fis[13] = 0;
    fis[14] = 0;
    fis[15] = 0;

    if (sata_io(fis, sizeof(fis), sata_dma, BSIZE, write) != 0)
    {
        release(&sata_lock);
        panic("sata: rw failed");
    }

    if (!write)
        memmove(b->data, sata_dma, BSIZE);

    release(&sata_lock);
}

#endif
