/* 裸机 SATA 测试：不含任何内核代码。
 * 复用 U-Boot 连续运行的端口引擎（不停止、不重写 CLB/FB），
 * 命令写进 U-Boot 自己的 slot0/cmd_tbl（缓存写，与 U-Boot 一致），
 * 数据缓冲用全新的 bank1 地址 0x90000000B8003000（此前从未用过）。
 * 若这里能通而内核挂，说明是内核状态破坏；若这里也挂，说明是
 * go 交接后的环境本身。 */
typedef unsigned char  u8;
typedef unsigned int   u32;
typedef unsigned long long u64;

#define UART_BASE (0x8000000000000000ULL | 0x1fe20000ULL)
#define HBA_BASE  (0x8000000000000000ULL | 0x400e0000ULL)
#define PORT_BASE 0x100
#define DATA_BUF  0x90000000B8003000ULL

#define REG_CLB  0x00
#define REG_CLBU 0x04
#define REG_FB   0x08
#define REG_IS   0x10
#define REG_CMD  0x18
#define REG_TFD  0x20
#define REG_SSTS 0x28
#define REG_SCTL 0x2c
#define REG_SERR 0x30
#define REG_CI   0x38

static inline u32 pr(u32 off)
{
    return *(volatile u32 *)((u64)HBA_BASE + PORT_BASE + off);
}
static inline void pw(u32 off, u32 v)
{
    *(volatile u32 *)((u64)HBA_BASE + PORT_BASE + off) = v;
    (void)*(volatile u32 *)((u64)HBA_BASE + PORT_BASE + off);
}

static inline void uart_putc(char c)
{
    volatile u8 *lsr = (volatile u8 *)(UART_BASE + 5);
    volatile u8 *thr = (volatile u8 *)(UART_BASE + 0);
    while (!(*lsr & 0x20))
        ;
    *thr = (u8)c;
}
static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}
static void uart_hex(u64 v, int d)
{
    int i;
    for (i = d - 1; i >= 0; i--)
    {
        int n = (int)((v >> (4 * i)) & 0xf);
        uart_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
    }
}

static u64 now(void)
{
    u64 val = 0;
    int rid = 0;
    __asm__ volatile("rdtime.d %0, %1" : "=r"(val), "=r"(rid));
    return val;
}

static void msleep_loop(u64 ms)
{
    u64 d0 = now();
    while (now() - d0 < ms * 125000ULL)
        ;
}

/* COMRESET 恢复端口（SCTL DET=1 -> 0，等链路和设备就绪） */
static void recover(void)
{
    u32 sctl = pr(REG_SCTL);

    pw(REG_SCTL, (sctl & ~0xfU) | 1U);
    msleep_loop(1);
    pw(REG_SCTL, sctl & ~0xfU);
    msleep_loop(200);
    pw(REG_SERR, pr(REG_SERR));
    pw(REG_IS, pr(REG_IS));
}

/* mode: 0 = U-Boot 式（1ms 轮询、不清 SERR/IS）
 *       1 = 不轮询（下发后睡 5 秒再查）
 *       2 = 密集轮询（不清 SERR/IS） */
static int issue(const u8 *fis, u64 data_buf, int mode)
{
    u32 clb_lo = pr(REG_CLB), clb_hi = pr(REG_CLBU);
    u64 clb = ((u64)clb_hi << 32) | clb_lo;
    volatile u32 *slot0;
    volatile u8 *ctp;
    u64 ct;
    u64 t0;
    int i;

    if (clb == 0)
        return -1;
    slot0 = (volatile u32 *)(u64)clb;
    ct = ((u64)slot0[3] << 32) | slot0[2];
    if (ct == 0)
        return -1;
    ctp = (volatile u8 *)(u64)ct;

    uart_puts("CLB=");
    uart_hex(clb >> 32, 8);
    uart_hex(clb & 0xffffffff, 8);
    uart_puts(" ct=");
    uart_hex(ct >> 32, 8);
    uart_hex(ct & 0xffffffff, 8);
    uart_puts(" buf=");
    uart_hex(data_buf >> 32, 8);
    uart_hex(data_buf & 0xffffffff, 8);
    uart_puts("\r\n");

    /* 清 slot0，写命令头（缓存写，与 U-Boot 一致） */
    for (i = 0; i < 8; i++)
        slot0[i] = 0;
    slot0[0] = 0x10005;                 /* cfl=5, prdtl=1 */
    slot0[1] = 0;
    slot0[2] = (u32)(ct & 0xffffffff);
    slot0[3] = (u32)(ct >> 32);

    /* 清 cmd_tbl，写 cfis + SG */
    for (i = 0; i < 0x90 / 4; i++)
        ((volatile u32 *)(u64)ct)[i] = 0;
    for (i = 0; i < 20; i++)
        ctp[i] = fis[i];
    {
        volatile u32 *sg = (volatile u32 *)(u64)(ct + 0x80);
        sg[0] = (u32)(data_buf & 0xffffffff);
        sg[1] = 0;          /* 32 位物理地址：2K1000LA 手册 6.4 节，
                             * DMA 地址必须能被内存控制器解码（0x9000
                             * 前缀只是 CPU 的 DMW 虚拟地址，不解码） */
        sg[2] = 0x1ff;
        uart_puts(" sg=");
        uart_hex(sg[1], 8);
        uart_hex(sg[0], 8);
        uart_puts("\r\n");
    }

    /* 与 U-Boot 一致：不清 SERR/IS，直接下发 */
    pw(REG_CI, 1);

    /* 轮询/等待最多 5 秒 */
    t0 = now();
    while (now() - t0 < 625000000ULL)
    {
        u32 ci = pr(REG_CI);
        if (!(ci & 1))
            break;
        if (mode == 0)
            msleep_loop(1);         /* U-Boot 式：每 1ms 读一次 */
        else if (mode == 1)
            msleep_loop(5000);      /* 不轮询：直接睡满再查 */
        /* mode 2：密集轮询（无延时） */
    }

    uart_puts("mode=");
    uart_hex(mode, 1);
    uart_puts(" CI=");
    uart_hex(pr(REG_CI), 8);
    uart_puts(" TFD=");
    uart_hex(pr(REG_TFD), 8);
    uart_puts(" IS=");
    uart_hex(pr(REG_IS), 8);
    uart_puts(" SERR=");
    uart_hex(pr(REG_SERR), 8);
    uart_puts(" hdr_st=");
    uart_hex(slot0[1], 8);
    uart_puts("\r\ndata=");
    for (i = 0; i < 4; i++)
    {
        uart_hex(((volatile u32 *)(u64)data_buf)[i], 8);
        uart_putc(' ');
    }
    uart_puts("\r\n");
    return (pr(REG_CI) & 1) ? -1 : 0;
}

void main(void)
{
    u8 fis[20];
    int i;

    uart_puts("\r\n[BM] bare-metal SATA test\r\n");
    uart_puts("CMD=");
    uart_hex(pr(REG_CMD), 8);
    uart_puts(" CI=");
    uart_hex(pr(REG_CI), 8);
    uart_puts(" TFD=");
    uart_hex(pr(REG_TFD), 8);
    uart_puts(" SSTS=");
    uart_hex(pr(REG_SSTS), 8);
    uart_puts("\r\n");

    /* 预填数据缓冲 0x55 */
    for (i = 0; i < 512 / 4; i++)
        ((volatile u32 *)(u64)DATA_BUF)[i] = 0x55555555;

    /* 1) IDENTIFY（PIO-in） */
    for (i = 0; i < 20; i++)
        fis[i] = 0;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = 0xec;
    uart_puts("[BM] IDENTIFY mode0 (uboot-poll)\r\n");
    issue(fis, DATA_BUF, 0);
    recover();

    /* 2) IDENTIFY mode1（不轮询） */
    for (i = 0; i < 512 / 4; i++)
        ((volatile u32 *)(u64)DATA_BUF)[i] = 0x55555555;
    for (i = 0; i < 20; i++)
        fis[i] = 0;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = 0xec;
    uart_puts("[BM] IDENTIFY mode1 (no-poll)\r\n");
    issue(fis, DATA_BUF, 1);
    recover();

    /* 3) IDENTIFY mode2（密集轮询） */
    for (i = 0; i < 512 / 4; i++)
        ((volatile u32 *)(u64)DATA_BUF)[i] = 0x55555555;
    for (i = 0; i < 20; i++)
        fis[i] = 0;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = 0xec;
    uart_puts("[BM] IDENTIFY mode2 (tight-poll)\r\n");
    issue(fis, DATA_BUF, 2);
    recover();

    /* 4) READ EXT sector 0（DMA-in，U-Boot 式轮询） */
    for (i = 0; i < 512 / 4; i++)
        ((volatile u32 *)(u64)DATA_BUF)[i] = 0x55555555;
    for (i = 0; i < 20; i++)
        fis[i] = 0;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = 0x25;
    fis[7] = 0x40;
    fis[12] = 1;
    uart_puts("[BM] READ EXT sector0 mode0\r\n");
    issue(fis, DATA_BUF, 0);

    uart_puts("[BM] DONE\r\n");
    while (1)
        ;
}
