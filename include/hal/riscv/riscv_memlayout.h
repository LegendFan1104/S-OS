// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// QEMU virt's Goldfish RTC, time value in nanoseconds since the Unix epoch.
#define GOLDFISH_RTC 0x101000L
#define GOLDFISH_RTC_TIME_LOW (GOLDFISH_RTC + 0x00)
#define GOLDFISH_RTC_TIME_HIGH (GOLDFISH_RTC + 0x04)

// virtio mmio interface
//#define VIRTIO0 0x10001000UL
#define VIRTIO0_IRQ 1

// StarFive JH7110 (VisionFive 2) SD/MMC 控制器
// microSD 卡槽 = SDIO1 @ 0x16020000，eMMC = SDIO0 @ 0x16010000
#define SDMMC0 0x16020000UL

// StarFive JH7110 系统时钟/复位控制寄存器（SYS_CRG）
// 时钟寄存器偏移 = 时钟 ID * 4；复位 assert @ 0x2F8, status @ 0x308
#define JH7110_SYS_CRG 0x13020000UL

// core local interruptor (CLINT), which contains the timer.
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // cycles since boot.

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACKSIZE              8*PGSIZE //< du命令要的内核栈空间比2个页面更多，加到4个页面
#define EXTRASIZE               16* PGSIZE
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* (KSTACKSIZE+EXTRASIZE))

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
