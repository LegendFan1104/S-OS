// sdmmc.c - VisionFive 2 (StarFive JH7110) microSD 驱动
// ------------------------------------------------------------------
// JH7110 的 MMC/SDIO 是 Synopsys DesignWare Mobile Storage Host (dw_mmc)：
//   microSD 卡槽 = SDIO1 @ 0x16020000（4 位总线）
//   eMMC        = SDIO0 @ 0x16010000（8 位总线）
//
// 本驱动移植自 U-Boot dw_mmc 驱动（drivers/mmc/dw_mmc.c, include/dwmmc.h），
// 寄存器位定义与 U-Boot/Linux 完全一致；采用 1 位总线 + FIFO(PIO) 传输，
// 不用 DMA、不用中断。
// 初始化序列：CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7；
// 读写：单块 CMD17/CMD24（每 512 字节一块）。
#include "types.h"
#include "print.h"
#include "defs.h"
#include "spinlock.h"
#include "string.h"
#include "buf.h"
#include "riscv_memlayout.h"

/* DW MMC 寄存器（字节偏移，同 U-Boot dwmmc.h） */
#define DW_CTRL     0x000
#define DW_PWREN    0x004
#define DW_CLKDIV   0x008
#define DW_CLKSRC   0x00c
#define DW_CLKENA   0x010
#define DW_TMOUT    0x014
#define DW_CTYPE    0x018
#define DW_BLKSIZ   0x01c
#define DW_BYTCNT   0x020
#define DW_INTMASK  0x024
#define DW_CMDARG   0x028
#define DW_CMD      0x02c
#define DW_RESP0    0x030
#define DW_RESP1    0x034
#define DW_RESP2    0x038
#define DW_RESP3    0x03c
#define DW_MINTSTS  0x040
#define DW_RINTSTS  0x044
#define DW_STATUS   0x048
#define DW_FIFOTH   0x04c
#define DW_CDETECT  0x050
#define DW_BMOD     0x080
#define DW_IDINTEN  0x090
#define DW_FIFO     0x200

/* CTRL */
#define DW_CTRL_RESET      (1U << 0)
#define DW_CTRL_FIFO_RESET (1U << 1)
#define DW_CTRL_DMA_RESET  (1U << 2)
#define DW_RESET_ALL       (DW_CTRL_RESET | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET)

/* CMD（与 U-Boot/Linux dw_mmc 一致） */
#define DW_CMD_INDX(n)       ((n) & 0x3fU)   /* 命令号是 6 位（0-63），CMD55 不能截断 */
#define DW_CMD_RESP_EXP      (1U << 6)
#define DW_CMD_RESP_LONG     (1U << 7)   /* 1=136 位响应，0=48 位 */
#define DW_CMD_RESP_CRC      (1U << 8)
#define DW_CMD_DATA_EXP      (1U << 9)
#define DW_CMD_RW            (1U << 10)
#define DW_CMD_SEND_STOP     (1U << 12)
#define DW_CMD_PRV_DAT_WAIT  (1U << 13)
#define DW_CMD_SEND_INIT     (1U << 15)
#define DW_CMD_UPD_CLK       (1U << 21)
#define DW_CMD_USE_HOLD_REG  (1U << 29)
#define DW_CMD_START         (1U << 31)

/* CLKENA */
#define DW_CLKEN_ENABLE      (1U << 0)
#define DW_CLKEN_LOW_PWR     (1U << 16)

/* RINTSTS / MINTSTS */
#define DW_INT_CD    (1U << 0)
#define DW_INT_RE    (1U << 1)
#define DW_INT_CDONE (1U << 2)
#define DW_INT_DTO   (1U << 3)
#define DW_INT_TXDR  (1U << 4)
#define DW_INT_RXDR  (1U << 5)
#define DW_INT_RCRC  (1U << 6)
#define DW_INT_DCRC  (1U << 7)
#define DW_INT_RTO   (1U << 8)
#define DW_INT_DRTO  (1U << 9)
#define DW_INT_HTO   (1U << 10)
#define DW_INT_FRUN  (1U << 11)
#define DW_INT_HLE   (1U << 12)
#define DW_INT_SBE   (1U << 13)
#define DW_INT_EBE   (1U << 15)

#define DW_INT_ERR (DW_INT_RE | DW_INT_RTO | DW_INT_DRTO | DW_INT_HTO | \
                    DW_INT_FRUN | DW_INT_HLE | DW_INT_SBE | DW_INT_EBE | \
                    DW_INT_RCRC | DW_INT_DCRC)

/* 响应阶段错误（命令是否成功只看这些） */
#define DW_INT_RESP_ERR (DW_INT_RE | DW_INT_RTO | DW_INT_RCRC)
/* 数据阶段错误（同 U-Boot DATA_ERR | DATA_TOUT） */
#define DW_INT_DATA_ERR (DW_INT_EBE | DW_INT_SBE | DW_INT_HLE | DW_INT_FRUN | \
                         DW_INT_DCRC | DW_INT_HTO | DW_INT_DRTO)

/* STATUS */
#define DW_STATUS_FIFO_EMPTY (1U << 2)
#define DW_STATUS_FIFO_FULL  (1U << 3)
#define DW_STATUS_BUSY       (1U << 9)

/* FIFOTH */
#define DW_FIFOTH_MSIZE(x)    ((uint32)(x) << 28)
#define DW_FIFOTH_RX_WMARK(x) ((uint32)(x) << 16)
#define DW_FIFOTH_TX_WMARK(x) ((uint32)(x))

/* JH7110 SYS_CRG 里 SDIO1 的时钟/复位（与 Linux/U-Boot 一致）：
 * 时钟寄存器偏移 = 时钟 ID * 4；SDIO1_AHB=92, SDIO1_SDCARD=94
 * 复位 SDIO1_AHB(id=65) -> assert 寄存器 0x300 的 bit 1，写 0 释放 */
#define JH7110_SDIO1_AHB      92U
#define JH7110_SDIO1_SDCARD   94U
#define JH7110_CLK_ENABLE     (1U << 31)
#define JH7110_SDIO1_RST_REG  (JH7110_SYS_CRG + 0x2F8 + (65U / 32U) * 4U)
#define JH7110_SDIO1_RST_BIT  (1U << (65U % 32U))

/* SD 命令 */
#define SD_CMD0_GO_IDLE           0
#define SD_CMD2_ALL_SEND_CID      2
#define SD_CMD3_SEND_REL_ADDR     3
#define SD_CMD7_SELECT_CARD       7
#define SD_CMD8_SEND_IF_COND      8
#define SD_CMD9_SEND_CSD          9
#define SD_CMD13_SEND_STATUS     13
#define SD_CMD16_SET_BLOCKLEN    16
#define SD_CMD17_READ_SINGLE     17
#define SD_CMD24_WRITE_SINGLE    24
#define SD_CMD55_APP_CMD         55
#define SD_ACMD41_SD_SEND_OP_COND 41

/* R1 状态位 */
#define R1_READY_FOR_DATA (1U << 8)

#define SDMMC_POLL_MAX 20000000ULL   /* MMIO 轮询很慢，20M 次约 2 秒，超时可见 */
#define SDMMC_IO_MAX   10000000ULL

static volatile uint32 *sdmmc_regs; /* 恒等映射的 MMIO */
static int sdmmc_ready = 0;
static int sdmmc_need_init = 0;     /* 下一个命令带 SEND_INIT（同 U-Boot） */
static uint32 sdmmc_rca = 0;        /* 卡的相对地址 */
static struct spinlock sdmmc_lock;

/* 对齐的扇区缓冲：FIFO 字访问与 b->data 的对齐无关 */
static uint32 sdmmc_sector[512 / 4] __attribute__((aligned(8)));

#define DW_REG32(off) (*(volatile uint32 *)((uint64)sdmmc_regs + (off)))

static void sdmmc_clear_int(void)
{
    DW_REG32(DW_RINTSTS) = 0xffffffffU;
}

/*
 * JH7110 上电后 SDIO1 外设时钟默认关闭且处于复位态，必须先使能。
 * SDCARD 时钟寄存器：bit31 使能，bits[3:0] 分频（值=分频数，1 不分频）。
 * 这里设 8，把 CIU 时钟压到约 50MHz（axi_cfg0 约 400MHz）。
 */
static void sdmmc_jh7110_clock_enable(void)
{
    volatile uint32 *crg = (volatile uint32 *)JH7110_SYS_CRG;
    volatile uint32 *rst = (volatile uint32 *)JH7110_SDIO1_RST_REG;

    /* 先开 AHB 门控时钟，再释放复位（Linux 复位驱动提示：
     * 时钟没开时解除复位可能一直等不到 status） */
    crg[JH7110_SDIO1_AHB] |= JH7110_CLK_ENABLE;
    __sync_synchronize();
    *rst &= ~JH7110_SDIO1_RST_BIT;
    __sync_synchronize();

    /* SDCARD 时钟：使能 + 分频 */
    crg[JH7110_SDIO1_SDCARD] = JH7110_CLK_ENABLE | 8U;
    __sync_synchronize();
}

/*
 * 切换卡时钟，完整执行 U-Boot dwmci_setup_bus 的握手：
 * 关 CLKENA -> 写 CLKDIV -> 发 UPD_CLK 命令 -> 开 CLKENA -> 再发一次。
 * 卡钟 = CIU / (2 * (CLKDIV + 1))（CLKDIV=0 时 = CIU/2）。
 */
static int sdmmc_set_clock(uint32 div)
{
    uint64 i;

    DW_REG32(DW_CLKENA) = 0;
    DW_REG32(DW_CLKSRC) = 0;
    DW_REG32(DW_CLKDIV) = div;
    DW_REG32(DW_CMD) =
        DW_CMD_PRV_DAT_WAIT | DW_CMD_UPD_CLK | DW_CMD_START;
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if (!(DW_REG32(DW_CMD) & DW_CMD_START))
            break;
    }
    if (i == SDMMC_POLL_MAX)
        return -1;

    DW_REG32(DW_CLKENA) = DW_CLKEN_ENABLE | DW_CLKEN_LOW_PWR;
    DW_REG32(DW_CMD) =
        DW_CMD_PRV_DAT_WAIT | DW_CMD_UPD_CLK | DW_CMD_START;
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if (!(DW_REG32(DW_CMD) & DW_CMD_START))
            break;
    }
    return (i == SDMMC_POLL_MAX) ? -1 : 0;
}

/*
 * 发送一条命令（同 U-Boot dwmci_send_cmd 的寄存器流程）。
 * extra 是 CMD 寄存器里除索引和 START/USE_HOLD_REG 外的位；
 * resp 非空时返回响应：
 *   48 位响应: resp[0] = RESP0
 *  136 位响应: resp[0..3] = RESP3/RESP2/RESP1/RESP0（高位在前）
 */
static int sdmmc_send_cmd(uint32 cmdindex, uint32 arg, uint32 extra,
                          uint32 *resp)
{
    uint64 i;
    uint32 sts = 0;
    uint32 cmd;

    /* 等控制器空闲（数据忙位清零） */
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if (!(DW_REG32(DW_STATUS) & DW_STATUS_BUSY))
            break;
    }
    if (i == SDMMC_POLL_MAX)
        return -1;

    sdmmc_clear_int();
    DW_REG32(DW_CMDARG) = arg;

    cmd = DW_CMD_PRV_DAT_WAIT | DW_CMD_INDX(cmdindex) |
          DW_CMD_USE_HOLD_REG | DW_CMD_START | extra;
    if (sdmmc_need_init)
    {
        cmd |= DW_CMD_SEND_INIT;
        sdmmc_need_init = 0;
    }
    DW_REG32(DW_CMD) = cmd;

    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_CDONE)
            break;
        if (sts & DW_INT_ERR)
            break;
    }
    if (i == SDMMC_POLL_MAX)
    {
        printf("[sdmmc] CMD%u timeout (RINTSTS=0x%x)\n", cmdindex, sts);
        return -1;
    }

    if (sts & DW_INT_ERR)
    {
        printf("[sdmmc] CMD%u error (RINTSTS=0x%x)\n", cmdindex, sts);
        DW_REG32(DW_RINTSTS) = sts & (DW_INT_CDONE | DW_INT_ERR);
        return -1;
    }

    /* 只清命令完成/错误位，数据阶段的 DTO/RXDR/TXDR 留给调用者 */
    DW_REG32(DW_RINTSTS) = DW_INT_CDONE;

    if (resp)
    {
        resp[0] = DW_REG32(DW_RESP0);
        if (extra & DW_CMD_RESP_LONG)
        {
            resp[3] = DW_REG32(DW_RESP0);
            resp[2] = DW_REG32(DW_RESP1);
            resp[1] = DW_REG32(DW_RESP2);
            resp[0] = DW_REG32(DW_RESP3);
        }
    }
    return 0;
}

/* 单块读：CMD17，FIFO(PIO) */
static int sdmmc_read_sector(uint32 sector, uchar *buf)
{
    /* RW 位(bit10)语义：0=读，1=写；读命令不置位 */
    uint32 cmd =
        DW_CMD_RESP_EXP | DW_CMD_RESP_CRC | DW_CMD_DATA_EXP |
        DW_CMD_PRV_DAT_WAIT |
        DW_CMD_INDX(SD_CMD17_READ_SINGLE) | DW_CMD_START |
        DW_CMD_USE_HOLD_REG;
    uint32 *p = (uint32 *)sdmmc_sector;
    int words = 512 / 4;
    int got = 0;
    uint64 i;
    uint32 sts = 0;

    /* 数据阶段前复位 FIFO（同 U-Boot） */
    DW_REG32(DW_CTRL) |= DW_CTRL_FIFO_RESET;
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if (!(DW_REG32(DW_CTRL) & DW_CTRL_FIFO_RESET))
            break;
    }

    DW_REG32(DW_BLKSIZ) = 512;
    DW_REG32(DW_BYTCNT) = 512;
    sdmmc_clear_int();
    DW_REG32(DW_CMDARG) = sector;
    DW_REG32(DW_CMD) = cmd;

    /* 等命令完成（数据阶段的中断位先不管，只看响应错误） */
    for (i = 0; i < SDMMC_IO_MAX; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_CDONE)
            break;
        if (sts & DW_INT_RESP_ERR)
            break;
    }
    if (i == SDMMC_IO_MAX || (sts & DW_INT_RESP_ERR))
    {
        printf("[sdmmc] read CMD17 error (RINTSTS=0x%x)\n", sts);
        return -1;
    }
    DW_REG32(DW_RINTSTS) = DW_INT_CDONE;

    /* 数据阶段：RXDR 时读 FIFO，直到 DTO */
    for (i = 0; i < SDMMC_IO_MAX && got < words; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_DATA_ERR)
        {
            printf("[sdmmc] read data error (RINTSTS=0x%x)\n", sts);
            return -1;
        }
        if (sts & DW_INT_RXDR)
        {
            while (got < words &&
                   !(DW_REG32(DW_STATUS) & DW_STATUS_FIFO_EMPTY))
                p[got++] = DW_REG32(DW_FIFO);
            DW_REG32(DW_RINTSTS) = DW_INT_RXDR;
        }
        if (sts & DW_INT_DTO)
            break;
    }
    /* DTO 后把 FIFO 剩余读完 */
    while (got < words && !(DW_REG32(DW_STATUS) & DW_STATUS_FIFO_EMPTY))
        p[got++] = DW_REG32(DW_FIFO);
    if (got < words)
    {
        printf("[sdmmc] read short (got %d/%d)\n", got, words);
        return -1;
    }

    /* 必须等到 DTO：确认本次数据阶段完整结束，否则下一个命令的
     * PRV_DAT_WAIT 会一直等前一次传输完成而响应超时（RTO） */
    for (; i < SDMMC_IO_MAX; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_DTO)
            break;
        if (sts & DW_INT_DATA_ERR)
        {
            printf("[sdmmc] read DTO wait error (RINTSTS=0x%x)\n", sts);
            return -1;
        }
    }
    if (i == SDMMC_IO_MAX)
    {
        printf("[sdmmc] read DTO timeout\n");
        return -1;
    }
    DW_REG32(DW_RINTSTS) = DW_INT_DTO | DW_INT_RXDR;

    memmove(buf, sdmmc_sector, 512);
    return 0;
}

/* 单块写：CMD24，FIFO(PIO) */
static int sdmmc_write_sector(uint32 sector, const uchar *buf)
{
    /* 写命令置 RW 位 */
    uint32 cmd =
        DW_CMD_RESP_EXP | DW_CMD_RESP_CRC | DW_CMD_DATA_EXP | DW_CMD_RW |
        DW_CMD_PRV_DAT_WAIT |
        DW_CMD_INDX(SD_CMD24_WRITE_SINGLE) | DW_CMD_START |
        DW_CMD_USE_HOLD_REG;
    const uint32 *p;
    int words = 512 / 4;
    int sent = 0;
    uint64 i;
    uint32 sts = 0;

    memmove(sdmmc_sector, buf, 512);
    p = (const uint32 *)sdmmc_sector;

    /* 等控制器/卡不忙（同 sdmmc_send_cmd，避免卡还在编程时发命令） */
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if (!(DW_REG32(DW_STATUS) & DW_STATUS_BUSY))
            break;
    }
    if (i == SDMMC_POLL_MAX)
        return -1;

    DW_REG32(DW_CTRL) |= DW_CTRL_FIFO_RESET;
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if (!(DW_REG32(DW_CTRL) & DW_CTRL_FIFO_RESET))
            break;
    }

    DW_REG32(DW_BLKSIZ) = 512;
    DW_REG32(DW_BYTCNT) = 512;
    sdmmc_clear_int();
    DW_REG32(DW_CMDARG) = sector;
    DW_REG32(DW_CMD) = cmd;

    /* 等命令完成；期间 TXDR 置位就补数据（同 U-Boot，不在命令前预填） */
    for (i = 0; i < SDMMC_IO_MAX; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_CDONE)
            break;
        if (sts & DW_INT_RESP_ERR)
            break;
        if ((sts & DW_INT_TXDR) && sent < words)
        {
            while (sent < words &&
                   !(DW_REG32(DW_STATUS) & DW_STATUS_FIFO_FULL))
                DW_REG32(DW_FIFO) = p[sent++];
            DW_REG32(DW_RINTSTS) = DW_INT_TXDR;
        }
    }
    if (i == SDMMC_IO_MAX || (sts & DW_INT_RESP_ERR))
    {
        printf("[sdmmc] write CMD24 error (RINTSTS=0x%x)\n", sts);
        return -1;
    }
    DW_REG32(DW_RINTSTS) = DW_INT_CDONE;

    for (i = 0; i < SDMMC_IO_MAX && sent < words; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_DATA_ERR)
        {
            printf("[sdmmc] write data error (RINTSTS=0x%x)\n", sts);
            return -1;
        }
        if (sts & DW_INT_TXDR)
        {
            while (sent < words &&
                   !(DW_REG32(DW_STATUS) & DW_STATUS_FIFO_FULL))
                DW_REG32(DW_FIFO) = p[sent++];
            DW_REG32(DW_RINTSTS) = DW_INT_TXDR;
        }
        if (sts & DW_INT_DTO)
            break;
    }

    /* 必须等到 DTO：确认数据已被卡接收（同 U-Boot dwmci_data_transfer） */
    for (; i < SDMMC_IO_MAX; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_DTO)
            break;
        if (sts & DW_INT_DATA_ERR)
        {
            printf("[sdmmc] write DTO wait error (RINTSTS=0x%x)\n", sts);
            return -1;
        }
    }
    if (i == SDMMC_IO_MAX)
    {
        printf("[sdmmc] write DTO timeout\n");
        return -1;
    }
    if (sent < words)
    {
        printf("[sdmmc] write short (sent %d/%d)\n", sent, words);
        return -1;
    }
    DW_REG32(DW_RINTSTS) = DW_INT_DTO | DW_INT_TXDR;

    /* 等卡编程完成：轮询 CMD13 直到 READY_FOR_DATA（比 STATUS busy 可靠，
     * 编程期间卡会忽略新的 CMD24，直接发会导致 RTO） */
    for (i = 0; i < 100000; i++)
    {
        uint32 r1;
        if (sdmmc_send_cmd(SD_CMD13_SEND_STATUS, sdmmc_rca << 16,
                           DW_CMD_RESP_EXP | DW_CMD_RESP_CRC, &r1) != 0)
            return -1;
        if (r1 & R1_READY_FOR_DATA)
            break;
    }
    if (i == 100000)
    {
        printf("[sdmmc] write card busy timeout\n");
        return -1;
    }
    return 0;
}

/* 从 R2(136 位) 响应里提取 CSD 字节（高位在前） */
static void sdmmc_csd_bytes(uint32 *resp, uint8 *csd)
{
    int i;
    for (i = 0; i < 16; i++)
    {
        uint32 word = resp[i / 4];
        csd[i] = (uint8)(word >> (24 - 8 * (i % 4)));
    }
}

static int sdmmc_card_init(void)
{
    uint32 resp[4];
    uint64 i;

    /* 控制器复位（同 U-Boot dwmci_init） */
    DW_REG32(DW_CTRL) = DW_RESET_ALL;
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if ((DW_REG32(DW_CTRL) & DW_RESET_ALL) == 0)
            break;
    }

    DW_REG32(DW_PWREN) = 1;
    DW_REG32(DW_CTYPE) = 0;              /* 1 位总线 */
    DW_REG32(DW_TMOUT) = 0xffffffffU;
    DW_REG32(DW_BMOD) = 1;
    DW_REG32(DW_IDINTEN) = 0;
    DW_REG32(DW_FIFOTH) =
        DW_FIFOTH_MSIZE(2) | DW_FIFOTH_RX_WMARK(15) | DW_FIFOTH_TX_WMARK(16);

    /* 以约 400kHz 枚举 */
    if (sdmmc_set_clock(62) != 0)
        return -1;

    DW_REG32(DW_RINTSTS) = 0xffffffffU;
    DW_REG32(DW_INTMASK) = 0;

    /* 第一个命令（CMD0）带 SEND_INIT，输出 80 个时钟 */
    sdmmc_need_init = 1;

    if (sdmmc_send_cmd(SD_CMD0_GO_IDLE, 0, 0, NULL) != 0)
        return -1;

    /* CMD8: SD 2.0 握手 */
    if (sdmmc_send_cmd(SD_CMD8_SEND_IF_COND, 0x1aa,
                       DW_CMD_RESP_EXP | DW_CMD_RESP_CRC, resp) != 0)
        return -1;
    if ((resp[0] & 0xfffU) != 0x1aaU)
    {
        printf("[sdmmc] CMD8 echo mismatch (0x%x)\n", resp[0] & 0xfffU);
        return -1;
    }

    /* ACMD41: 上电循环直到卡就绪（HCS 置位，3.0~3.3V） */
    for (i = 0; i < 1000; i++)
    {
        if (sdmmc_send_cmd(SD_CMD55_APP_CMD, sdmmc_rca << 16,
                           DW_CMD_RESP_EXP | DW_CMD_RESP_CRC, NULL) != 0)
            return -1;
        /* ACMD41 响应是 R3(OCR)，无 CRC 字段，不能要求校验 CRC */
        if (sdmmc_send_cmd(SD_ACMD41_SD_SEND_OP_COND, 0x40ff8000U,
                           DW_CMD_RESP_EXP, resp) != 0)
            return -1;
        if (resp[0] & 0x80000000U)
            break;
    }
    if (i == 1000)
    {
        printf("[sdmmc] ACMD41 not ready\n");
        return -1;
    }

    /* 切到高速时钟（CIU 约 50MHz / 4 = 12.5MHz） */
    if (sdmmc_set_clock(1) != 0)
        return -1;

    /* CMD2: CID（136 位响应） */
    if (sdmmc_send_cmd(SD_CMD2_ALL_SEND_CID, 0,
                       DW_CMD_RESP_EXP | DW_CMD_RESP_LONG | DW_CMD_RESP_CRC,
                       resp) != 0)
        return -1;

    /* CMD3: 获取 RCA */
    if (sdmmc_send_cmd(SD_CMD3_SEND_REL_ADDR, 0,
                       DW_CMD_RESP_EXP | DW_CMD_RESP_CRC, resp) != 0)
        return -1;
    sdmmc_rca = (resp[0] >> 16) & 0xffffU;

    /* CMD9: CSD，读取容量（信息用，失败不影响使用） */
    if (sdmmc_send_cmd(SD_CMD9_SEND_CSD, sdmmc_rca << 16,
                       DW_CMD_RESP_EXP | DW_CMD_RESP_LONG | DW_CMD_RESP_CRC,
                       resp) == 0)
    {
        uint8 csd[16];
        sdmmc_csd_bytes(resp, csd);
        if ((csd[0] >> 6) == 0x1U) /* CSD v2.0 */
        {
            uint64 c_size = ((uint64)(csd[7] & 0x3fU) << 16) |
                            ((uint64)csd[8] << 8) | (uint64)csd[9];
            printf("[sdmmc] SDHC/SDXC, ~%d MB\n",
                   (uint32)((c_size + 1) / 2)); /* (C_SIZE+1)*512KB */
        }
    }

    /* CMD7: 选中卡 */
    if (sdmmc_send_cmd(SD_CMD7_SELECT_CARD, sdmmc_rca << 16,
                       DW_CMD_RESP_EXP | DW_CMD_RESP_CRC, resp) != 0)
        return -1;
    if (!(resp[0] & R1_READY_FOR_DATA))
    {
        printf("[sdmmc] card not ready (R1=0x%x)\n", resp[0]);
        return -1;
    }

    /* CMD16: 块长 512（SDHC 固定 512，写了无害） */
    sdmmc_send_cmd(SD_CMD16_SET_BLOCKLEN, 512,
                   DW_CMD_RESP_EXP | DW_CMD_RESP_CRC, NULL);

    printf("[sdmmc] SD card ready: RCA=0x%x\n", sdmmc_rca);
    return 0;
}

void sdmmc_init(void)
{
    initlock(&sdmmc_lock, "sdmmc");
    sdmmc_regs = (volatile uint32 *)SDMMC0;
    printf("[sdmmc] init JH7110 SD/MMC @0x%x\n", (uint32)SDMMC0);

    /* 使能 SDIO1 时钟并释放复位 */
    sdmmc_jh7110_clock_enable();

    if (sdmmc_card_init() != 0)
    {
        printf("[sdmmc] init failed\n");
        return;
    }
    sdmmc_ready = 1;
}

/* 块设备读写入口，与 virtio_rw 同签名，由 bio.c 调用 */
int sdmmc_rw(struct buf *b, int write)
{
    uint32 sector;
    uint32 i;

    if (!sdmmc_ready)
        panic("sdmmc: disk not ready");

    acquire(&sdmmc_lock);
    sector = (uint32)(b->blockno * (BSIZE / 512));
    if (write)
    {
        for (i = 0; i < BSIZE / 512; i++)
        {
            if (sdmmc_write_sector(sector + i, b->data + i * 512) != 0)
            {
                release(&sdmmc_lock);
                panic("sdmmc: write failed");
            }
        }
    }
    else
    {
        for (i = 0; i < BSIZE / 512; i++)
        {
            if (sdmmc_read_sector(sector + i, b->data + i * 512) != 0)
            {
                release(&sdmmc_lock);
                panic("sdmmc: read failed");
            }
        }
    }
    release(&sdmmc_lock);
    return 0;
}
