// sdmmc.c - VisionFive 2 (StarFive JH7110) microSD 驱动
// ------------------------------------------------------------------
// JH7110 的 MMC/SDIO 是 Synopsys DesignWare Mobile Storage Host (dw_mmc)：
//   microSD 卡槽 = SDIO1 @ 0x16020000（4 位总线）
//   eMMC        = SDIO0 @ 0x16010000（8 位总线）
// Linux 参考: drivers/mmc/host/dw_mmc.c + dw_mmc-starfive.c
//
// 本驱动只做最简功能：
//   - 1 位总线 + FIFO(PIO) 传输，不用 DMA、不用中断
//   - 初始化: CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7
//   - 读写: 单块 CMD17/CMD24（每 512 字节一块）
// 块大小按文件系统 BSIZE(4096)=8 个扇区逐块循环。
#include "types.h"
#include "print.h"
#include "defs.h"
#include "spinlock.h"
#include "string.h"
#include "buf.h"
#include "riscv_memlayout.h"

/* DW MMC 寄存器（字节偏移） */
#define DW_CTRL     0x00
#define DW_PWREN    0x04
#define DW_CLKDIV   0x08
#define DW_CLKSRC   0x0c
#define DW_CLKENA   0x10
#define DW_TMOUT    0x14
#define DW_CTYPE    0x18
#define DW_BLKSIZ   0x1c
#define DW_BYTCNT   0x20
#define DW_INTMASK  0x24
#define DW_CMDARG   0x28
#define DW_CMD      0x2c
#define DW_RESP0    0x30
#define DW_RESP1    0x34
#define DW_RESP2    0x38
#define DW_RESP3    0x3c
#define DW_MINTSTS  0x40
#define DW_RINTSTS  0x44
#define DW_STATUS   0x48
#define DW_FIFOTH   0x4c
#define DW_CDETECT  0x50
#define DW_WRTPRT   0x54
#define DW_DEBNCE   0x64
#define DW_FIFO     0x80

/* CTRL */
#define DW_CTRL_ENABLE     (1U << 0)
#define DW_CTRL_RESET      (1U << 1)
#define DW_CTRL_FIFO_RESET (1U << 2)
#define DW_CTRL_DMA_RESET  (1U << 3)

/* CMD */
#define DW_CMD_START         (1U << 0)
#define DW_CMD_INDEX_SHIFT   1
#define DW_CMD_RESP_EXPECT   (1U << 6)
#define DW_CMD_RESP_LEN_136  (1U << 7)     /* bits[9:7] = 0b001 */
#define DW_CMD_RESP_LEN_48   (2U << 7)     /* bits[9:7] = 0b010 */
#define DW_CMD_CHECK_CRC     (1U << 10)
#define DW_CMD_DATA_EXPECT   (1U << 11)
#define DW_CMD_READ          (1U << 12)
#define DW_CMD_SEND_INIT     (1U << 18)

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

/* STATUS */
#define DW_STATUS_FIFO_FULL  (1U << 11)
#define DW_STATUS_FIFO_EMPTY (1U << 13)

/* SD 命令 */
#define SD_CMD0_GO_IDLE           0
#define SD_CMD2_ALL_SEND_CID      2
#define SD_CMD3_SEND_REL_ADDR     3
#define SD_CMD7_SELECT_CARD       7
#define SD_CMD8_SEND_IF_COND      8
#define SD_CMD9_SEND_CSD          9
#define SD_CMD16_SET_BLOCKLEN    16
#define SD_CMD17_READ_SINGLE     17
#define SD_CMD24_WRITE_SINGLE    24
#define SD_CMD55_APP_CMD         55
#define SD_ACMD41_SD_SEND_OP_COND 41

/* R1 状态位 */
#define R1_READY_FOR_DATA (1U << 8)

#define SDMMC_POLL_MAX 200000000ULL
#define SDMMC_IO_MAX   100000000ULL

static volatile uint32 *sdmmc_regs; /* 恒等映射的 MMIO */
static int sdmmc_ready = 0;
static uint32 sdmmc_rca = 0;        /* 卡的相对地址 */
static struct spinlock sdmmc_lock;

/* 对齐的扇区缓冲：FIFO 字访问与 b->data 的对齐无关 */
static uint32 sdmmc_sector[512 / 4] __attribute__((aligned(8)));

#define DW_REG32(off) (*(volatile uint32 *)((uint64)sdmmc_regs + (off)))

static void sdmmc_clear_int(void)
{
    DW_REG32(DW_RINTSTS) = 0xffffffffU;
}

/* 设分频并切换时钟（输出 ≈ 输入 / (2*(div+1))，输入约 50MHz） */
static void sdmmc_set_clock(uint32 div)
{
    DW_REG32(DW_CLKENA) &= ~1U;
    __sync_synchronize();
    DW_REG32(DW_CLKDIV) = div & 0xffU;
    DW_REG32(DW_CLKENA) = 1U;
    __sync_synchronize();
}

/*
 * 发送一条命令并等待命令完成。
 * extra 是 CMD 寄存器除索引和 START 外的位；resp 非空时返回响应：
 *   48 位响应: resp[0] = RESP0（即响应位[39:8]）
 *  136 位响应: resp[0..3] = RESP3/RESP2/RESP1/RESP0（高位在前）
 */
static int sdmmc_send_cmd(uint32 cmdindex, uint32 arg, uint32 extra,
                          uint32 *resp)
{
    uint64 i;
    uint32 sts = 0;

    sdmmc_clear_int();
    DW_REG32(DW_CMDARG) = arg;
    DW_REG32(DW_CMD) =
        (cmdindex << DW_CMD_INDEX_SHIFT) | extra | DW_CMD_START;

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
        return -1;
    }

    if (resp)
    {
        resp[0] = DW_REG32(DW_RESP0);
        if (extra & DW_CMD_RESP_LEN_136)
        {
            resp[1] = DW_REG32(DW_RESP1);
            resp[2] = DW_REG32(DW_RESP2);
            resp[3] = DW_REG32(DW_RESP3);
        }
    }
    DW_REG32(DW_RINTSTS) = DW_INT_CDONE | DW_INT_ERR;
    return 0;
}

/* 单块读：CMD17，FIFO(PIO) */
static int sdmmc_read_sector(uint32 sector, uchar *buf)
{
    uint32 cmd =
        (SD_CMD17_READ_SINGLE << DW_CMD_INDEX_SHIFT) |
        DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 | DW_CMD_CHECK_CRC |
        DW_CMD_DATA_EXPECT | DW_CMD_READ | DW_CMD_START;
    uint32 *p = (uint32 *)sdmmc_sector;
    int words = 512 / 4;
    int got = 0;
    uint64 i;
    uint32 sts = 0;

    DW_REG32(DW_BLKSIZ) = 512;
    DW_REG32(DW_BYTCNT) = 512;
    sdmmc_clear_int();
    DW_REG32(DW_CMDARG) = sector;
    DW_REG32(DW_CMD) = cmd;

    /* 等命令完成 */
    for (i = 0; i < SDMMC_IO_MAX; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_CDONE)
            break;
        if (sts & DW_INT_ERR)
            break;
    }
    if (i == SDMMC_IO_MAX || (sts & DW_INT_ERR))
    {
        printf("[sdmmc] read CMD17 error (RINTSTS=0x%x)\n", sts);
        return -1;
    }

    /* 数据阶段：RXDR 时读 FIFO，直到 DTO */
    for (i = 0; i < SDMMC_IO_MAX && got < words; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_ERR)
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
    DW_REG32(DW_RINTSTS) = DW_INT_DTO | DW_INT_RXDR;

    memmove(buf, sdmmc_sector, 512);
    return 0;
}

/* 单块写：CMD24，FIFO(PIO) */
static int sdmmc_write_sector(uint32 sector, const uchar *buf)
{
    uint32 cmd =
        (SD_CMD24_WRITE_SINGLE << DW_CMD_INDEX_SHIFT) |
        DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 | DW_CMD_CHECK_CRC |
        DW_CMD_DATA_EXPECT | DW_CMD_START;
    const uint32 *p;
    int words = 512 / 4;
    int sent = 0;
    uint64 i;
    uint32 sts = 0;

    memmove(sdmmc_sector, buf, 512);
    p = (const uint32 *)sdmmc_sector;

    DW_REG32(DW_BLKSIZ) = 512;
    DW_REG32(DW_BYTCNT) = 512;
    sdmmc_clear_int();
    DW_REG32(DW_CMDARG) = sector;
    DW_REG32(DW_CMD) = cmd;

    for (i = 0; i < SDMMC_IO_MAX; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_CDONE)
            break;
        if (sts & DW_INT_ERR)
            break;
    }
    if (i == SDMMC_IO_MAX || (sts & DW_INT_ERR))
    {
        printf("[sdmmc] write CMD24 error (RINTSTS=0x%x)\n", sts);
        return -1;
    }

    for (i = 0; i < SDMMC_IO_MAX && sent < words; i++)
    {
        sts = DW_REG32(DW_RINTSTS);
        if (sts & DW_INT_ERR)
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
    if (sent < words)
    {
        printf("[sdmmc] write short (sent %d/%d)\n", sent, words);
        return -1;
    }
    DW_REG32(DW_RINTSTS) = DW_INT_DTO | DW_INT_TXDR;
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

    /* 控制器复位 */
    DW_REG32(DW_CTRL) =
        DW_CTRL_RESET | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET;
    for (i = 0; i < SDMMC_POLL_MAX; i++)
    {
        if ((DW_REG32(DW_CTRL) &
             (DW_CTRL_RESET | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET)) == 0)
            break;
    }
    DW_REG32(DW_CTRL) = DW_CTRL_ENABLE;
    DW_REG32(DW_PWREN) = 1;
    DW_REG32(DW_CTYPE) = 0;              /* 1 位总线 */
    DW_REG32(DW_TMOUT) = 0xffffffffU;
    DW_REG32(DW_FIFOTH) = 16 | (16U << 16);
    DW_REG32(DW_INTMASK) =
        DW_INT_CDONE | DW_INT_DTO | DW_INT_TXDR | DW_INT_RXDR | DW_INT_ERR;
    DW_REG32(DW_DEBNCE) = 0xffffU;
    sdmmc_set_clock(62);                 /* 约 400kHz */

    /* CMD0: 进 idle */
    if (sdmmc_send_cmd(SD_CMD0_GO_IDLE, 0, DW_CMD_SEND_INIT, NULL) != 0)
        return -1;

    /* CMD8: SD 2.0 握手 */
    if (sdmmc_send_cmd(SD_CMD8_SEND_IF_COND, 0x1aa,
                       DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 |
                           DW_CMD_CHECK_CRC,
                       resp) != 0)
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
                           DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 |
                               DW_CMD_CHECK_CRC,
                           NULL) != 0)
            return -1;
        if (sdmmc_send_cmd(SD_ACMD41_SD_SEND_OP_COND, 0x40ff8000U,
                           DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 |
                               DW_CMD_CHECK_CRC,
                           resp) != 0)
            return -1;
        if (resp[0] & 0x80000000U)
            break;
    }
    if (i == 1000)
    {
        printf("[sdmmc] ACMD41 not ready\n");
        return -1;
    }

    /* 切到高速时钟（约 12~25MHz） */
    sdmmc_set_clock(1);

    /* CMD2: CID（136 位响应） */
    if (sdmmc_send_cmd(SD_CMD2_ALL_SEND_CID, 0,
                       DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_136 |
                           DW_CMD_CHECK_CRC,
                       resp) != 0)
        return -1;

    /* CMD3: 获取 RCA */
    if (sdmmc_send_cmd(SD_CMD3_SEND_REL_ADDR, 0,
                       DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 |
                           DW_CMD_CHECK_CRC,
                       resp) != 0)
        return -1;
    sdmmc_rca = (resp[0] >> 16) & 0xffffU;

    /* CMD9: CSD，读取容量（信息用，失败不影响使用） */
    if (sdmmc_send_cmd(SD_CMD9_SEND_CSD, sdmmc_rca << 16,
                       DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_136 |
                           DW_CMD_CHECK_CRC,
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
                       DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 |
                           DW_CMD_CHECK_CRC,
                       resp) != 0)
        return -1;
    if (!(resp[0] & R1_READY_FOR_DATA))
    {
        printf("[sdmmc] card not ready (R1=0x%x)\n", resp[0]);
        return -1;
    }

    /* CMD16: 块长 512（SDHC 固定 512，写了无害） */
    sdmmc_send_cmd(SD_CMD16_SET_BLOCKLEN, 512,
                   DW_CMD_RESP_EXPECT | DW_CMD_RESP_LEN_48 |
                       DW_CMD_CHECK_CRC,
                   NULL);

    printf("[sdmmc] SD card ready: RCA=0x%x\n", sdmmc_rca);
    return 0;
}

void sdmmc_init(void)
{
    initlock(&sdmmc_lock, "sdmmc");
    sdmmc_regs = (volatile uint32 *)SDMMC0;
    printf("[sdmmc] init JH7110 SD/MMC @0x%x\n", (uint32)SDMMC0);

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
