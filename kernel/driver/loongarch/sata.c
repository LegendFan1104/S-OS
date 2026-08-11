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
 * 移植自 U-Boot drivers/ata/ahci.c（2K1000 的 U-Boot `sata` 命令同源），
 * 寄存器偏移/位定义与 U-Boot include/ahci.h、Linux drivers/ata/ahci.h
 * 一致；流程对应：
 *   ahci_reset / ahci_host_init / ahci_link_up / wait_spinup /
 *   ahci_port_start / ahci_fill_sg / ahci_fill_cmd_slot /
 *   ahci_device_data_io
 * 采用轮询完成（PxCI 位清 0），不依赖中断。
 *
 * 2K1000 的 SATA 控制器是 SoC 内部 PCI 设备：
 *   vendor = 0x0014 (Loongson), device = 0x7a08, class = 0x010601
 *   Linux 设备树: /bus@10000000/pci@1a000000/sata@8,0 (device 8, fn 0)
 *   PCI 配置空间(ECAM)物理基址: 0x1a000000（可用 -DSATA_PCIE_ECAM_PHYS= 覆盖）
 *   AHCI HBA 的 ABAR 在 PCI BAR5（AHCI 规范，Linux/U-Boot 均取 BAR5），
 *   经 DMW1 (0x8000...) 非缓存窗口访问。
 */

#define LA_DMW0_MASK 0x9000000000000000ULL
#define LA_DMW1_MASK 0x8000000000000000ULL

/* 直映射窗口别名互转：设备 DMA 直接读写物理内存，CPU 必须通过
 * DMW1 非缓存别名访问共享结构，避免缓存脏数据掩盖设备写入。 */
#define LA_UNCACHED(ptr) \
    ((void *)(((uint64)(ptr) & ~LA_DMW0_MASK) | LA_DMW1_MASK))
/* 由窗口别名得到物理地址（剥掉 0x9000/0x8000 窗口位） */
#define PA2VA(ptr) ((uint64)(ptr) & ~(LA_DMW0_MASK))

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

/* 忙等上限（近似；2K1000 主频约 1GHz） */
#define SATA_POLL_MAX 200000000ULL
#define SATA_IO_MAX   100000000ULL

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
static uint32 sata_cap = 0;
static struct spinlock sata_lock;

/* 与 HBA 共享的 DMA 结构：CPU 访问必须走 LA_UNCACHED 别名，
 * 硬件地址用 PA2VA(...) 得到物理地址。 */
static uint8 sata_cmdlist[AHCI_CMD_SLOT_SZ * AHCI_MAX_CMD_SLOT]
    __attribute__((aligned(2048)));
static uint8 sata_fis[AHCI_RX_FIS_SZ] __attribute__((aligned(256)));
static uint8 sata_cmdtbl[AHCI_CMD_TBL_HDR + 16] __attribute__((aligned(128)));
static uchar sata_dma[BSIZE] __attribute__((aligned(64)));
static uchar sata_ident[512] __attribute__((aligned(64)));

#define HBA32(off) (*(volatile uint32 *)((uint64)sata_hba + (off)))
#define PORT32(off) \
    (*(volatile uint32 *)((uint64)sata_hba + PORT_BASE + \
                          ((uint64)sata_port * PORT_STRIDE) + (off)))

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
/* AHCI 初始化（对应 U-Boot ahci_reset + ahci_host_init）               */
/* ------------------------------------------------------------------ */

static int sata_hba_reset(void)
{
    uint32 tmp;
    uint64 i;

    tmp = HBA32(HOST_CTL);
    if ((tmp & HOST_RESET) == 0)
        HBA32(HOST_CTL) = tmp | HOST_RESET;

    /* 复位必须完成，否则硬件视为故障 */
    for (i = 0; i < SATA_POLL_MAX && (HBA32(HOST_CTL) & HOST_RESET); i++)
        ;
    if (i == SATA_POLL_MAX)
    {
        printf("[sata] controller reset failed (0x%x)\n", HBA32(HOST_CTL));
        return -1;
    }
    return 0;
}

/* 等待 SATA 链路就绪（DET == PHYRDY） */
static int sata_link_up(void)
{
    uint64 i;
    for (i = 0; i < SATA_POLL_MAX; i++)
    {
        if ((PORT32(PORT_SCR_STAT) & PORT_SCR_STAT_DET_MASK) ==
            PORT_SCR_STAT_DET_PHYRDY)
            return 0;
    }
    return -1;
}

/* 等待设备 spinup 完成（TFDATA 不再 busy） */
static int sata_wait_spinup(void)
{
    uint64 i;
    for (i = 0; i < SATA_POLL_MAX; i++)
    {
        if (!(PORT32(PORT_TFDATA) & (ATA_BUSY | ATA_DRQ)))
            return 0;
    }
    return -1;
}

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

    HBA32(HOST_CTL) = HOST_AHCI_EN;
    HBA32(HOST_CAP) = cap_save;
    /* U-Boot 对部分控制器强制写端口实现位再读回 */
    HBA32(HOST_PORTS_IMPL) = 0xf;
    sata_cap = HBA32(HOST_CAP);

    for (i = 0; i < 32; i++)
    {
        uint32 port_map = HBA32(HOST_PORTS_IMPL);

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
            PORT32(PORT_CMD) = tmp;
            /* 规范要求 500ms/位，这里轮询等待引擎停 */
            sata_wait_clear(&PORT32(PORT_CMD),
                            PORT_CMD_LIST_ON | PORT_CMD_FIS_ON,
                            SATA_POLL_MAX);
        }

        /* spin up + 等待链路 */
        tmp = PORT32(PORT_CMD);
        tmp |= PORT_CMD_SPIN_UP;
        PORT32(PORT_CMD) = tmp;

        if (sata_link_up() != 0)
        {
            printf("[sata] SATA link %d timeout\n", i);
            continue;
        }

        /* 清错误并等设备就绪 */
        tmp = PORT32(PORT_SCR_ERR);
        if (tmp)
            PORT32(PORT_SCR_ERR) = tmp;
        if (sata_wait_spinup() != 0)
        {
            printf("[sata] port %d spinup timeout\n", i);
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

    printf("[sata] no link on any port\n");
    return -1;
}

/* 对应 U-Boot ahci_port_start */
static int sata_port_start(void)
{
    uint32 port_status;

    port_status = PORT32(PORT_SCR_STAT);
    if ((port_status & PORT_SCR_STAT_DET_MASK) !=
        PORT_SCR_STAT_DET_PHYRDY)
    {
        printf("[sata] no link on port %d (SSTS=0x%x)\n",
               sata_port, port_status);
        return -1;
    }

    memset(LA_UNCACHED(sata_cmdlist), 0, sizeof(sata_cmdlist));
    memset(LA_UNCACHED(sata_fis), 0, sizeof(sata_fis));
    memset(LA_UNCACHED(sata_cmdtbl), 0, sizeof(sata_cmdtbl));

    PORT32(PORT_LST_ADDR) = (uint32)PA2VA(LA_UNCACHED(sata_cmdlist));
    PORT32(PORT_LST_ADDR_HI) = 0;
    PORT32(PORT_FIS_ADDR) = (uint32)PA2VA(LA_UNCACHED(sata_fis));
    PORT32(PORT_FIS_ADDR_HI) = 0;

    PORT32(PORT_CMD) = PORT_CMD_ICC_ACTIVE | PORT_CMD_FIS_RX |
                       PORT_CMD_POWER_ON | PORT_CMD_SPIN_UP |
                       PORT_CMD_START;
    __sync_synchronize();

    return sata_wait_spinup();
}

/* ------------------------------------------------------------------ */
/* 命令提交（对应 U-Boot ahci_fill_sg + ahci_fill_cmd_slot +           */
/* ahci_device_data_io）                                               */
/* ------------------------------------------------------------------ */

static int sata_fill_sg(struct ahci_cmd_tbl *tbl, uchar *buf, int buf_len)
{
    struct ahci_sg *sg = &tbl->sg[0];
    uint64 pa = PA2VA(LA_UNCACHED(buf));
    int sg_count = ((buf_len - 1) / MAX_DATA_BYTE_COUNT) + 1;
    int i;

    if (sg_count > AHCI_MAX_SG)
        return -1;

    for (i = 0; i < sg_count; i++)
    {
        sg[i].addr = (uint32)(pa & 0xffffffffU);
        sg[i].addr_hi = (uint32)(pa >> 32);
        if (sg[i].addr_hi && !(sata_cap & (1U << 31)))
        {
            printf("[sata] DMA address too high\n");
            return -1;
        }
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
 *   buf       数据缓冲（物理内存，走 DMW1 非缓存别名）
 *   buf_len   数据字节数
 *   is_write  1 = 写盘（H2D），0 = 读盘
 */
static int sata_io(uint8 *fis, int fis_len, uchar *buf, int buf_len,
                   int is_write)
{
    struct ahci_cmd_hdr *hdr =
        (struct ahci_cmd_hdr *)LA_UNCACHED(sata_cmdlist);
    struct ahci_cmd_tbl *tbl =
        (struct ahci_cmd_tbl *)LA_UNCACHED(sata_cmdtbl);
    uint32 port_status, opts;
    int sg_count;
    uint64 i;

    port_status = PORT32(PORT_SCR_STAT);
    if ((port_status & PORT_SCR_STAT_DET_MASK) !=
        PORT_SCR_STAT_DET_PHYRDY)
        return -1;

    memset(tbl, 0, sizeof(*tbl));
    memmove(LA_UNCACHED(tbl->cfis), fis, fis_len);

    sg_count = sata_fill_sg(tbl, buf, buf_len);
    if (sg_count < 0)
        return -1;

    /* opts：cfl(命令 FIS 长度，dword) | prdtl<<16 | W(写)<<6
     * 与 U-Boot ahci_device_data_io 完全一致 */
    opts = (uint32)(fis_len >> 2) | ((uint32)sg_count << 16) |
           (is_write ? (1U << 6) : 0);
    hdr->opts = opts;
    hdr->status = 0;
    hdr->tbl_addr = (uint32)PA2VA(LA_UNCACHED(sata_cmdtbl));
    hdr->tbl_addr_hi = 0;
    __sync_synchronize();

    PORT32(PORT_IRQ_STAT) = 0xffffffffU;
    PORT32(PORT_CMD_ISSUE) = 1U << SATA_SLOT;

    /* 忙等命令完成（PxCI 槽位清 0） */
    for (i = 0; i < SATA_IO_MAX; i++)
    {
        if ((PORT32(PORT_CMD_ISSUE) & (1U << SATA_SLOT)) == 0)
            break;
    }
    __sync_synchronize();

    if (i == SATA_IO_MAX)
    {
        printf("[sata] io timeout: cmd=0x%x (CI=0x%x TFD=0x%x)\n",
               fis[2], PORT32(PORT_CMD_ISSUE), PORT32(PORT_TFDATA));
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
    return 0;
}

/* ------------------------------------------------------------------ */
/* IDENTIFY DEVICE：读磁盘容量等信息                                    */
/* ------------------------------------------------------------------ */

static int sata_identify(void)
{
    uint8 fis[20];
    volatile uint16 *id = (volatile uint16 *)LA_UNCACHED(sata_ident);
    uint64 lba48;
    uint32 lba28;

    memset(fis, 0, sizeof(fis));
    fis[0] = 0x27;         /* H2D FIS */
    fis[1] = 1 << 7;       /* C：Command FIS */
    fis[2] = ATA_CMD_ID_ATA;

    if (sata_io(fis, sizeof(fis), sata_ident, 512, 0) != 0)
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

    /* 2. AHCI ABAR 在 BAR5（AHCI 规范，Linux/U-Boot 均用 BAR5）；
     *    个别实现为空时回退扫描其他内存 BAR。 */
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

    /* 4. 映射 HBA 并初始化 */
    sata_hba = (volatile uint32 *)(LA_DMW1_MASK | bar_phys);
    if (HBA32(HOST_CAP) == 0xffffffffU || HBA32(HOST_CAP) == 0)
    {
        printf("[sata] HBA invalid: CAP=0x%x (BAR=0x%x)\n",
               HBA32(HOST_CAP), bar_phys);
        sata_hba = NULL;
        return;
    }
    printf("[sata] HBA at 0x%x\n", bar_phys);

    /* 5. 主机初始化 + 端口启动 + IDENTIFY */
    if (sata_host_init() != 0 || sata_port_start() != 0 ||
        sata_identify() != 0)
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
        memmove(LA_UNCACHED(sata_dma), b->data, BSIZE);

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
        memmove(b->data, LA_UNCACHED(sata_dma), BSIZE);

    release(&sata_lock);
}

#endif
