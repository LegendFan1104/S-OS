# 架构层与 Trap 设计

项目：SOS (SuperOS)

队伍 ID：T2026104869910625

本文档记录 SOS 在 RISC-V64 与 LoongArch64 双架构下的 HAL 层、寄存器保存、异常入口、中断入口和系统调用入口设计。当前代码已经具备完整的双架构 trap entry 汇编、trap frame 保存恢复和 C 语言分发入口。

## 设计目标

- 保持 C + Assembly 的内核方向。
- 架构相关代码集中在 `hal/` 和 `hsai/`，HAL 层暴露稳定的 C 接口给上层内核。
- 使用统一的 trap frame 结构和分发语义承接两套架构入口，包括 exception、external interrupt、timer interrupt 与 syscall。
- 双架构通过编译期 `#if defined RISCV` / `#else` (LoongArch) 条件编译，避免运行时分支。

## 当前已有文件分析

### RISC-V64 HAL (`hal/riscv/`)

| 文件 | 职责 | 当前状态 |
|------|------|----------|
| [entry.S](hal/riscv/entry.S) | 内核入口，初始化栈指针，跳转 `sos_start_kernel` | ✅ 已稳定 |
| [start.c](hal/riscv/start.c) | 机器模式启动（无 SBI 时），设置 mtimecmp | ✅ 已有，SBI 路径不经过 |
| [switch.S](hal/riscv/switch.S) | 上下文切换，保存/恢复 ra、sp、s0-s11 | ✅ 已稳定 |
| [trampoline.S](hal/riscv/trampoline.S) | 用户态↔内核态切换蹦床，`uservec`/`userret` | ✅ 已稳定 |
| [kernelvec.S](hal/riscv/kernelvec.S) | 内核态中断/异常向量，分发到 `kerneltrap()` | ✅ 已稳定 |
| [sbi.c](hal/riscv/sbi.c) | OpenSBI ecall 封装（timer、console） | ✅ 已稳定 |
| [uart.c](hal/riscv/uart.c) | NS16550 UART 字符输入输出 | ✅ 已稳定 |

**RISC-V64 关键寄存器使用**：

| CSR | 用途 |
|-----|------|
| `stvec` | 内核态 trap 向量基址 → `kernelvec` |
| `sscratch` | 用户态 trap 时保存内核栈指针 |
| `sstatus` | SIE（中断使能）、SPIE（保存的中断使能）、SPP（trap 前特权级） |
| `sepc` | trap 前 PC，syscall 后 +4，`sret` 返回 |
| `scause` | trap 原因，最高位区分 interrupt/exception |
| `stval` | 地址异常等附加信息 |
| `satp` | 地址空间根页表（Sv39 模式） |

**RISC-V64 syscall ABI**：编号 `a7`，参数 `a0-a5`，返回值 `a0`。

### LoongArch64 HAL (`hal/loongarch/`)

| 文件 | 职责 | 当前状态 |
|------|------|----------|
| [entry.S](hal/loongarch/entry.S) | 内核入口，初始化 CSR，跳转 `sos_start_kernel` | ✅ 已稳定 |
| [kernelvec.S](hal/loongarch/kernelvec.S) | 内核态异常向量，支持 TLB 重填和通用异常分发 | ✅ 已稳定 |
| [merrvec.S](hal/loongarch/merrvec.S) | 机器错误异常处理向量 | ✅ 已稳定 |
| [swtch.S](hal/loongarch/swtch.S) | 上下文切换，保存/恢复 LA 寄存器（含 `$r21`、`$fp`） | ✅ 已稳定 |
| [tlbrefill.S](hal/loongarch/tlbrefill.S) | TLB 缺失重填处理器 | ✅ 已稳定 |
| [trampoline.S](hal/loongarch/trampoline.S) | 用户态↔内核态切换蹦床，`ertn` 返回 | ✅ 已稳定 |
| [uart.c](hal/loongarch/uart.c) | LoongArch 平台 UART 驱动 | ✅ 已稳定 |

**LoongArch64 关键寄存器使用**：

| CSR | 用途 |
|-----|------|
| `crmd` | 当前模式（PGDL/PGDH 选择）和全局中断状态（IE） |
| `prmd` | trap 前模式（PPLV）和中断状态（PIE），`ertn` 恢复来源 |
| `ecfg` | 异常和中断配置（LIE 局部中断使能位） |
| `estat` | 异常状态（Ecode/ESubcode）和中断 pending 位 |
| `era` | trap 前 PC，syscall 后 +4，`ertn` 返回目标 |
| `badv` | 地址异常的 fault address |
| `eentry` | 异常入口基址（需 4 KiB 对齐） |
| `pgdl`/`pgdh` | 页表根地址（低半区/高半区） |
| `tcfg`/`tval`/`ticlr` | 定时器配置、当前值、中断清除 |
| `save0` | trap entry scratch，保存 trap 前 `$sp` 并交换内核 trap stack |

**LoongArch64 syscall ABI**：编号 `$a7`，参数 `$a0-$a5`，返回值 `$a0`，与 RISC-V64 保持公共语义一致。

### HSAI Trap 分发 (`hsai/hsai_trap.c`)

`hsai_trap_init()` 负责安装异常向量：
- RISC-V64：写 `stvec` 指向 `kernelvec`（内核态）和 `uservec`（用户态 trampoline）
- LoongArch64：写 `eentry` 指向 trap entry（需 4 KiB 对齐），配置 `ecfg` 为 direct vector mode

**trap 分发流程**：

```
硬件 trap
  → HAL trampoline (uservec/kernelvec)
    → 保存通用寄存器 + CSR 到 trapframe
    → 切换到内核页表/栈
  → hsai_trap.c: usertrap() / kerneltrap()
    → 读取 scause/estat 判断 trap 类型
    ├─ scause=8 (syscall) → syscall() (kernel/syscall.c)
    ├─ 定时器中断         → yield() (时间片调度)
    ├─ 设备中断           → devintr()
    │   ├─ UART 中断      → uartintr() → consoleintr()
    │   ├─ PLIC 中断      → plic_claim() → virtio_disk_intr()
    │   └─ LoongArch      → 直接中断处理
    ├─ 缺页异常           → 页错误处理 / SIGSEGV
    └─ 其他异常           → panic / 杀死进程
  → usertrapret()
    → 设置 sepc/era
  → HAL trampoline (userret)
    → 恢复寄存器
    → sret / ertn 返回用户态
```

### Trap Frame 结构

RISC-V64 和 LoongArch64 各自定义 `struct trapframe`，通过 `copytrapframe()` 统一拷贝。关键字段：

| 字段 | RISC-V64 | LoongArch64 | 说明 |
|------|----------|-------------|------|
| `epc`/`era` | `sepc` | `era` | trap 前/返回 PC |
| `ra` | `x1` | `$r1` | 返回地址 |
| `sp` | `x2` | `$r3` | 栈指针 |
| `a0-a7` | `x10-x17` | `$r4-$r11` | 参数/返回值/syscall 编号 |
| `kernel_sp` | 内核栈顶 | 内核栈顶 | 内核态栈指针 |
| `kernel_satp`/`kernel_pgdl` | `satp` | `pgdl` | 内核页表基址 |
| `kernel_trap` | `stvec` | `eentry` | 内核 trap 向量地址 |

### 上下文切换

`hsai_swtch()` 在 `scheduler()` 中被调用，执行 `cpu->context` ↔ `proc->context` 的切换：

- RISC-V64：`switch.S` 保存/恢复 `ra`, `sp`, `s0-s11`
- LoongArch64：`swtch.S` 保存/恢复 `ra`, `sp`, `$r22`(fp), `$r23-$r31`(s0-s8)

## 双架构差异对比

| 方面 | RISC-V64 (rv64gc) | LoongArch64 (lp64d) |
|------|-------------------|---------------------|
| 页表级别 | 3 级 (Sv39) | 4 级 |
| 最大虚拟地址 | `1<<38` | `1<<31` |
| 系统调用指令 | `ecall` | `syscall 0` |
| 异常返回指令 | `sret` | `ertn` |
| MMIO 总线 | virtio-mmio | virtio-pci |
| 直接映射窗口 | 无（需逐页映射） | DMW0/DMW1（大窗口，`dmwin_win0` 或运算） |
| 内核基址 | `0x80200000` (OpenSBI) | `0x9000000000000000` |
| 特权模式 | M→S (OpenSBI) | 直接内核模式 |
| 帧指针寄存器 | `s0` | `fp` (`$r22`) |
| 中断控制器 | PLIC | 内置中断控制器 |
| 定时器 | SBI timer / mtimecmp | CPU-local timer CSR (TCFG/TVAL/TICLR) |

## 条件编译策略

整个内核使用统一的模式处理架构差异：

```c
#if defined RISCV
    p->trapframe->epc = program_entry;
    mappages(pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);
#else
    p->trapframe->era = program_entry;
    // LoongArch 使用 DMW 窗口，无需逐页映射 MMIO
#endif
```

编译时通过 `Makefile` 传入 `-DRISCV=1` 宏，顶层变量 `RISCV_CFLAGS` / `CFLAGS` 各自维护。

## 与其他子系统的依赖关系

- `trap` ← `sched`：定时器中断触发 `yield()` 和时间片统计。
- `trap` ← `syscall`：syscall trap 调用 `syscall()` 分发器。
- `trap` ← `mm`：缺页异常需要查询页表和 VMA。
- `trap` ← `drivers`：设备中断分发到 UART/VirtIO 处理。
- `arch` → `config`：页大小、栈大小影响 trap frame 位置。

## 后续开发任务

- RISC-V64 和 LoongArch64 的 CSR 语义、异常编号不同，公共代码不能假设 raw cause 编号一致。
- `TrapFrame` 字段偏移由汇编和 C 共同依赖，结构调整需同步更新汇编中的偏移常量。
- syscall 入口必须推进 PC（+4），否则会死循环执行同一条指令。
- LoongArch64 `EENTRY` 必须 4 KiB 对齐；`eentry` 写入需清低 12 位。
- kernel stack size 必须覆盖最坏 trap 嵌套。
