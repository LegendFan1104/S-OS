#if defined RISCV
// RISC-V 构建不编译本文件（与 virtio_disk.c 相同的约定）

#else

#include "types.h"
#include "print.h"
#include "defs.h"
#include "spinlock.h"
#include "string.h"
#include "buf.h"

/*
 * 2K1000 板上 SATA 驱动（AHCI 1.3）
 * ------------------------------------------------------------------
 * 2K1000 的 SATA 控制器是 SoC 内部 PCI 设备：
 *   vendor = 0x0014 (Loongson), device = 0x7a08, class = 0x010601
 *   Linux 设备树: /bus@10000000/pci@1a000000/sata@8,0 (device 8, fn 0)
 *   PCI 配置空间(ECAM)物理基址: 0x1a000000（可用板子自带 Linux 的
 *   /proc/iomem 核对，如不同用 -DSATA_PCIE_ECAM_PHYS=0x... 覆盖）
 *   BAR 指向的 AHCI HBA 寄存器经 DMW1 (0x8000...) 非缓存窗口访问
 *
 * 参考: AHCI 1.3 规范、Linux drivers/ata/ahci.c + ahci_loongson.c
 * 与 QEMU virtio 不同，本驱动采用轮询完成（与 la_virtio_disk_rw 一致），
 * 不依赖外部中断，便于真板先跑通。
 */

#define LA_DMW0_MASK 0x9000000000000000ULL
#define LA_DMW1_MASK 0x8000000000000000ULL

/* 直映射窗口别名互转（与 virtio_disk.c 相同）：
 * 设备 DMA 直接读写物理内存，CPU 必须通过 DMW1 非缓存别名访问
 * 共享结构，避免缓存脏数据掩盖设备写入。 */
#define LA_UNCACHED(ptr) \
    ((void *)(((uint64)(ptr) & ~LA_DMW0_MASK) | LA_DMW1_MASK))
/* 由窗口别名得到物理地址（剥掉 0x9000/0x8000 窗口位） */
#define PA2VA(pa) ((uint64)(pa) & ~(LA_DMW0_MASK))

#ifndef SATA_PCIE_ECAM_PHYS
#define SATA_PCIE_ECAM_PHYS 0x1a000000UL
#endif

#define LS2K_SATA_VENDOR 0x0014
#define LS2K_SATA_DEVICE 0x7a08
#define LS2K_SATA_DEV    8
#define LS2K_SATA_FN     0

/* AHCI 全局寄存器（字节偏移） */
#define AHCI_CAP 0x00
#define AHCI_GHC 0x04
#define AHCI_IS  0x08
#define AHCI_PI  0x0c
#define AHCI_VS  0x10

#define AHCI_PORT_BASE   0x100
#define AHCI_PORT_STRIDE 0x80

/* AHCI 端口寄存器（相对端口基址的字节偏移） */
#define PX_CLB  0x00
#define PX_CLBU 0x04
#define PX_FB   0x08
#define PX_FBU  0x0c
#define PX_IS   0x10
#define PX_IE   0x14
#define PX_CMD  0x18
#define PX_TFD  0x20
#define PX_SIG  0x24
#define PX_SSTS 0x28
#define PX_SCTL 0x2c
#define PX_SERR 0x30
#define PX_CI   0x38

#define GHC_HR (1U << 0)
#define GHC_IE (1U << 1)
#define GHC_AE (1U << 31)

#define PX_CMD_ST  (1U << 0)
#define PX_CMD_SUD (1U << 1)
#define PX_CMD_POD (1U << 2)
#define PX_CMD_FRE (1U << 4)
#define PX_CMD_FR  (1U << 14)
#define PX_CMD_CR  (1U << 15)

#define PX_IS_TFES (1U << 30)

#define PX_TFD_ERR (1U << 0)
#define PX_TFD_BSY (1U << 7)
#define PX_TFD_DRQ (1U << 3)

#define PX_SSTS_DET_MASK 0x0fU
#define PX_SSTS_DET_DEV  0x03U

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

/* 忙等上限（近似；2K1000 主频约 1GHz，1 亿次循环约几百毫秒） */
#define SATA_POLL_MAX 200000000ULL
#define SATA_IO_MAX   100000000ULL

/* 只使用命令槽 0，一次一条命令 */
#define SATA_SLOT 0

struct ahci_cmd_hdr {
    uint32 dw0;      /* bits4:0 cfl; bit5 a; bit6 w; bit7 p; bit8 c; bit9 r */
    uint32 dw1;      /* bits15:0 prdtl */
    uint32 ctba;     /* command table base address (低32位) */
    uint32 ctbau;
    uint32 rsvd[4];
};

struct ahci_prd {
    uint32 dba;
    uint32 dbau;
    uint32 rsvd;
    uint32 dbc;      /* bit31 i; bits21:0 字节数-1 */
};

struct ahci_cmd_tbl {
    uint8 cfis[64];
    uint8 acmd[16];
    uint8 rsvd[48];
    struct ahci_prd prd[1];
};

static volatile uint32 *sata_hba;   /* AHCI HBA，DMW1 非缓存别名 */
static volatile uint32 *sata_ecam;  /* PCI 配置空间，DMW1 非缓存别名 */
static uint32 sata_cfg_base;        /* 找到的设备的配置偏移 */
static int sata_ready = 0;
static int sata_port = 0;
static uint64 sata_nsectors = 0;    /* 512 字节扇区数 */
static uint32 sata_irq = 0;
static struct spinlock sata_lock;

/* 与 HBA 共享的 DMA 结构：CPU 访问必须走 LA_UNCACHED 别名，
 * 硬件地址用 PA2VA(...) 得到物理地址。 */
static uint8 sata_cmdlist[1024] __attribute__((aligned(1024)));
static uint8 sata_cmdtbl[256] __attribute__((aligned(128)));
static uint8 sata_fis[256] __attribute__((aligned(256)));
static uchar sata_dma[BSIZE] __attribute__((aligned(64)));
static uchar sata_ident[512] __attribute__((aligned(64)));

#define SATA_REG32(off) \
    (*(volatile uint32 *)((uint64)sata_hba + (off)))
#define SATA_PORT32(off) \
    (*(volatile uint32 *)((uint64)sata_hba + AHCI_PORT_BASE + \
                          ((uint64)sata_port * AHCI_PORT_STRIDE) + (off)))

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
    __sync_synchronize();
}

/* ------------------------------------------------------------------ */
/* 忙等辅助函数                                                        */
/* ------------------------------------------------------------------ */

static int sata_wait_clear(volatile uint32 *reg, uint32 mask, uint64 max)
{
    uint64 i;
    for (i = 0; i < max; i++)
    {
        if ((*reg & mask) == 0)
            return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* AHCI 端口初始化                                                     */
/* ------------------------------------------------------------------ */

static int sata_port_init(void)
{
    uint32 pi = SATA_REG32(AHCI_PI);
    uint32 cmd;

    if (pi == 0 || pi == 0xffffffffU)
    {
        printf("[sata] no port implemented (PI=0x%x)\n", pi);
        return -1;
    }

    /* 取最低的已实现端口 */
    sata_port = 0;
    while (!(pi & (1U << sata_port)))
        sata_port++;

    /* 停止端口 */
    cmd = SATA_PORT32(PX_CMD);
    SATA_PORT32(PX_CMD) = cmd & ~(PX_CMD_ST | PX_CMD_FRE);
    if (sata_wait_clear(&SATA_PORT32(PX_CMD), PX_CMD_CR | PX_CMD_FR,
                        SATA_POLL_MAX) != 0)
    {
        printf("[sata] port %d cannot stop\n", sata_port);
        return -1;
    }

    /* 命令列表与 FIS 接收区 */
    memset(LA_UNCACHED(sata_cmdlist), 0, sizeof(sata_cmdlist));
    memset(LA_UNCACHED(sata_fis), 0, sizeof(sata_fis));
    SATA_PORT32(PX_CLB) = (uint32)PA2VA(LA_UNCACHED(sata_cmdlist));
    SATA_PORT32(PX_CLBU) = 0;
    SATA_PORT32(PX_FB) = (uint32)PA2VA(LA_UNCACHED(sata_fis));
    SATA_PORT32(PX_FBU) = 0;

    /* PHY 复位并清错误 */
    SATA_PORT32(PX_SCTL) = 1;
    SATA_PORT32(PX_SCTL) = 0;
    SATA_PORT32(PX_SERR) = 0xffffffffU;
    SATA_PORT32(PX_IS) = 0xffffffffU;

    /* 等待设备接入 (DET=3) */
    {
        uint64 i;
        for (i = 0; i < SATA_POLL_MAX; i++)
        {
            if ((SATA_PORT32(PX_SSTS) & PX_SSTS_DET_MASK) == PX_SSTS_DET_DEV)
                break;
        }
        if (i == SATA_POLL_MAX)
        {
            printf("[sata] no device on port %d (SSTS=0x%x)\n",
                   sata_port, SATA_PORT32(PX_SSTS));
            return -1;
        }
    }

    /* 打开端口（轮询模式，屏蔽中断） */
    SATA_PORT32(PX_CMD) |= PX_CMD_POD | PX_CMD_SUD;
    SATA_PORT32(PX_CMD) |= PX_CMD_FRE;
    SATA_PORT32(PX_IE) = 0;
    SATA_PORT32(PX_IS) = 0xffffffffU;
    SATA_PORT32(PX_CMD) |= PX_CMD_ST;
    __sync_synchronize();

    printf("[sata] port %d ready\n", sata_port);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 命令提交（slot 0，忙等完成）                                        */
/* ------------------------------------------------------------------ */

/*
 * 在 slot 0 上发起一条命令并等待完成。
 *   cmd        ATA 命令码
 *   write      1 = 数据方向 H2D（写盘），0 = 读盘
 *   lba        48 位扇区号
 *   scount     扇区数（1..65535）
 *   prd_phys   PRD 数据缓冲物理地址（0 表示无数据阶段）
 *   dbc        数据字节数（PRD 字节数-1 由此计算）
 */
static int sata_issue(uint8 cmd, int write, uint64 lba, uint16 scount,
                      uint32 prd_phys, uint32 dbc)
{
    struct ahci_cmd_hdr *hdr =
        (struct ahci_cmd_hdr *)LA_UNCACHED(sata_cmdlist);
    struct ahci_cmd_tbl *tbl =
        (struct ahci_cmd_tbl *)LA_UNCACHED(sata_cmdtbl);
    uint8 *cfis;
    uint64 i;

    /* 等待端口空闲 */
    if (sata_wait_clear(&SATA_PORT32(PX_TFD), PX_TFD_BSY | PX_TFD_DRQ,
                        SATA_IO_MAX) != 0)
    {
        printf("[sata] port busy before issue (TFD=0x%x)\n",
               SATA_PORT32(PX_TFD));
        return -1;
    }

    memset(hdr, 0, sizeof(*hdr));
    memset(tbl, 0, sizeof(*tbl));

    /* H2D Register FIS - 设备寄存器 FIS */
    cfis = tbl->cfis;
    cfis[0] = 0x27;                              /* FIS 类型 H2D */
    cfis[1] = 0x80 | (write ? 0x40 : 0);         /* C=1, W=写方向 */
    cfis[2] = cmd;
    cfis[3] = 0;
    cfis[4] = (uint8)(lba);
    cfis[5] = (uint8)(lba >> 8);
    cfis[6] = (uint8)(lba >> 16);
    cfis[7] = 0x40;                              /* LBA 模式 */
    cfis[8] = (uint8)(lba >> 24);
    cfis[9] = (uint8)(lba >> 32);
    cfis[10] = (uint8)(lba >> 40);
    cfis[11] = 0;
    cfis[12] = (uint8)scount;
    cfis[13] = (uint8)(scount >> 8);
    cfis[14] = 0;
    cfis[15] = 0;

    if (prd_phys && dbc)
    {
        struct ahci_prd *prd = &tbl->prd[0];
        prd->dba = prd_phys;
        prd->dbau = 0;
        prd->rsvd = 0;
        prd->dbc = dbc - 1;
    }

    /* 命令头：cfl=5 (20B FIS)，写命令置 W，置 C(清 BSY) */
    hdr->dw0 = 5 | (write ? (1U << 6) : 0) | (1U << 8);
    hdr->dw1 = (prd_phys && dbc) ? 1 : 0;
    hdr->ctba = (uint32)PA2VA(LA_UNCACHED(sata_cmdtbl));
    hdr->ctbau = 0;
    __sync_synchronize();

    SATA_PORT32(PX_IS) = 0xffffffffU;
    SATA_PORT32(PX_CI) = 1U << SATA_SLOT;

    /* 忙等命令完成 */
    for (i = 0; i < SATA_IO_MAX; i++)
    {
        if ((SATA_PORT32(PX_CI) & (1U << SATA_SLOT)) == 0)
            break;
    }
    __sync_synchronize();

    if (i == SATA_IO_MAX)
    {
        printf("[sata] io timeout: cmd=0x%x (CI=0x%x TFD=0x%x)\n",
               cmd, SATA_PORT32(PX_CI), SATA_PORT32(PX_TFD));
        return -1;
    }
    if (SATA_PORT32(PX_IS) & PX_IS_TFES)
    {
        printf("[sata] task file error: cmd=0x%x (TFD=0x%x)\n",
               cmd, SATA_PORT32(PX_TFD));
        return -1;
    }
    if (SATA_PORT32(PX_TFD) & PX_TFD_ERR)
    {
        printf("[sata] tfd error: cmd=0x%x (TFD=0x%x)\n",
               cmd, SATA_PORT32(PX_TFD));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* IDENTIFY DEVICE：读磁盘容量等信息                                    */
/* ------------------------------------------------------------------ */

static int sata_identify(void)
{
    if (sata_issue(ATA_CMD_IDENTIFY, 0, 0, 0,
                   (uint32)PA2VA(LA_UNCACHED(sata_ident)), 512) != 0)
        return -1;

    if (SATA_PORT32(PX_SIG) != 0x00000101U)
    {
        printf("[sata] unexpected signature 0x%x\n", SATA_PORT32(PX_SIG));
        return -1;
    }

    {
        volatile uint16 *id = (volatile uint16 *)LA_UNCACHED(sata_ident);
        uint64 lba48 = (uint64)id[100] | ((uint64)id[101] << 16) |
                       ((uint64)id[102] << 32) | ((uint64)id[103] << 48);
        uint32 lba28 = (uint32)id[60] | ((uint32)id[61] << 16);
        sata_nsectors = lba48 ? lba48 : lba28;
    }

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

    /* 2. 取第一个非 IO 内存 BAR（AHCI HBA） */
    for (i = 0; i < 6; i++)
    {
        uint32 v = sata_cfg_read32(0x10 + 4 * i);
        if (v == 0 || (v & 0x1U))
            continue;
        bar_phys = v & ~0xfU;
        if (v & 0x4U)
        {
            uint32 hi = sata_cfg_read32(0x10 + 4 * (i + 1));
            bar_phys |= (hi & 0xfffffff0U);
            i++;
        }
        break;
    }
    if (bar_phys == 0)
    {
        printf("[sata] no memory BAR\n");
        return;
    }
    sata_irq = sata_cfg_read32(0x3c) & 0xffU;

    /* 3. 使能 IO/内存/总线主控 */
    cmd = sata_cfg_read16(0x04);
    sata_cfg_write16(0x04, (uint16)(cmd | 0x7U));

    /* 4. 映射 HBA 并复位 */
    sata_hba = (volatile uint32 *)(LA_DMW1_MASK | bar_phys);
    if (SATA_REG32(AHCI_CAP) == 0xffffffffU || SATA_REG32(AHCI_CAP) == 0)
    {
        printf("[sata] HBA invalid: CAP=0x%x (BAR=0x%x)\n",
               SATA_REG32(AHCI_CAP), bar_phys);
        sata_hba = NULL;
        return;
    }
    printf("[sata] HBA at 0x%x, irq %d\n", bar_phys, sata_irq);

    SATA_REG32(AHCI_GHC) |= GHC_HR;
    if (sata_wait_clear(&SATA_REG32(AHCI_GHC), GHC_HR, SATA_POLL_MAX) != 0)
    {
        printf("[sata] HBA reset timeout\n");
        sata_hba = NULL;
        return;
    }
    SATA_REG32(AHCI_GHC) |= GHC_AE;
    __sync_synchronize();

    /* 5. 端口初始化 + IDENTIFY */
    if (sata_port_init() != 0 || sata_identify() != 0)
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
    uint16 count;
    int rc;

    if (!sata_ready)
        panic("sata: disk not ready");

    acquire(&sata_lock);
    sector = (uint64)b->blockno * (BSIZE / 512);
    count = BSIZE / 512;

    if (write)
        memmove(LA_UNCACHED(sata_dma), b->data, BSIZE);

    rc = sata_issue(write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                    write, sector, count,
                    (uint32)PA2VA(LA_UNCACHED(sata_dma)), BSIZE);
    if (rc != 0)
    {
        release(&sata_lock);
        panic("sata: rw failed");
    }

    if (!write)
        memmove(b->data, LA_UNCACHED(sata_dma), BSIZE);

    release(&sata_lock);
}

#endif
