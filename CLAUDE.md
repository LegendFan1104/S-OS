# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

SOS（SuperOS）是基于 MIT XV6 的 C 语言教学操作系统，支持 **LoongArch64** 和 **RISC-V 64** 双架构。已通过全国大学生计算机系统能力大赛初赛的 Basic、Busybox、Libctest 和 Libcbench 测例。

## 构建与运行

**所有构建命令必须在 Docker 容器内执行。** 先通过 `docker exec -it sos3 bash` 进入容器，容器内已配置好所有工具链。

### 编译

```bash
make all              # 一次生成两种架构的内核镜像：kernel-la（LoongArch）和 kernel-rv（RISC-V）
make clean            # 清理所有构建产物
```

### 测试

测评机在项目根目录运行 `make all` 生成两个内核二进制文件，然后分别用以下命令启动测试：

**RISC-V 测试：**
```bash
qemu-system-riscv64 -machine virt -kernel kernel-rv -m 1G -nographic -smp 1 -bios default \
  -drive file=sdcard-rv.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -no-reboot \
  -device virtio-net-device,netdev=net \
  -netdev user,id=net \
  -rtc base=utc \
  -drive file=tmp/fs.img,if=none,format=raw,id=x1 \
  -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
```

**LoongArch 测试：**
```bash
qemu-system-loongarch64 -kernel kernel-la -m 1G -nographic -smp 1 \
  -drive file=sdcard-la.img,if=none,format=raw,id=x0 \
  -device virtio-blk-pci,drive=x0 \
  -no-reboot \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0 \
  -rtc base=utc \
  -drive file=disk-la.img,if=none,format=raw,id=x1 \
  -device virtio-blk-pci,drive=x1
```

- `sdcard-la.img` / `sdcard-rv.img`：两个架构下的测试用例磁盘镜像
- 测试耗时很长（20 分钟以上），**非必要不测试**，仅在用户明确要求或涉及关键路径修改时才运行
- 编译输出很长会消耗大量 token，**只关注报错部分即可**

## 架构

### 目录结构

```
hal/              # 硬件抽象层 — 架构相关的启动/入口/异常陷入/上下文切换
  loongarch/        # LoongArch: entry.S, trampoline.S, kernelvec.S, swtch.S, tlbrefill.S, uart.c
  riscv/            # RISC-V: entry.S, trampoline.S, kernelvec.S, switch.S, start.c, uart.c, sbi.c
hsai/             # HSAI（硬件-软件抽象接口）— 异常处理、UART/PLIC、内存
kernel/           # 架构无关的内核代码 + driver/{loongarch,riscv}/ 下的架构相关驱动
  driver/           # loongarch/: pci.c, virtio_disk.c, virtio_pci.c  |  riscv/: virt.c
  fs/               # ext4 文件系统实现 + VFS 层（ext4, vfat）
user/             # 用户态 initcode（作为 PID 1 加载的最小程序）
include/          # 所有头文件，目录结构与源码树对应

sdcard-la.img     #loongarc测试用例
sdcard-rv.img     #riscv测试用例
```

### 多架构策略

架构选择通过代码中 **编译期 `#ifdef RISCV`** 实现。Makefile 在 RISC-V 构建时设置 `-DRISCV=1`；LoongArch 不设此标志（视为默认）。HSAI 层通过统一 API 抽象架构差异（`hsai_trap_init`、`hsai_set_trapframe_*`、`hsai_swtch` 等）。

### 启动流程

1. **QEMU** 加载内核并跳转到 `hal/<arch>/entry.S` 中的 `_entry`
2. Entry 阶段：设置 DMW（LoongArch）/ 跳转到 `start`（RISC-V），配置每 CPU 栈，清零 BSS，然后调用 `kernel/sos_start_kernel.c` 中的 `sos_start_kernel()`
3. `sos_start_kernel()` 按顺序初始化各子系统：UART → 线程 → 进程 → 物理内存 (`pmem`) → 虚拟内存 (`vmem`) → slab 分配器 → 异常处理 → 磁盘 (VirtIO) → 文件系统 (ext4) → 创建首个用户进程 (`init_process`) → 进入**调度器**

### 进程与线程模型

- **进程**（`proc_t`，见 `process.h`）：最多 `NPROC=16`，每个进程有页表、VMA 链表、文件表、信号处理和线程队列。状态转换：UNUSED → USED → RUNNABLE → RUNNING → SLEEPING/ZOMBIE。
- **线程**（`thread_t`，见 `thread.h`）：池大小为 `THREAD_NUM=1024`。每个进程至少有一个主线程；通过 `clone(CLONE_VM)` 创建额外线程。每个线程有自己的内核栈和上下文。
- **调度器**（`process.c` 中的 `scheduler()`）：简单的轮询调度，永不返回（`__attribute__((noreturn))`）。通过 `yield()` / `sched()` 实现主动上下文切换。

### 用户态/内核态切换（Trampoline）

Trampoline 页（`trampoline.S`）被映射到用户页表和内核页表的相同虚拟地址。陷入流程：用户寄存器保存到进程的 `trapframe` → 加载内核页表 → `usertrap()` 处理异常 → `usertrapret()` 准备返回 → trampoline 中的 `userret` 恢复用户上下文并执行 `ertn`/`sret`。

### 虚拟内存

- `vmem.c` 提供页表遍历、映射（`mappages`）、分配（`uvmalloc`）以及用户态/内核态之间的 copy-in/out
- LoongArch 使用基于 DMW 的等值映射（虚拟地址高位置 `0x9`）—— 参见 `vmem.h` 中的 `to_vir()`/`to_phy()`
- RISC-V 使用标准 Sv39 页表，配合 `mcmodel=medany`

### 文件系统

- **VFS 层**（`fs.h`, `fs.c`）通过 `filesystem_op_t` 函数指针支持可插拔文件系统
- **ext4** 是主文件系统，由 `kernel/fs/ext4*.c` 实现标准 ext4 磁盘数据结构。VFS 操作位于 `kernel/fs/vfs_ext4.c`
- **vfat** 有骨架支持（`vfs_vfat.c`）
- 块 I/O 经过缓冲区缓存（`bio.c`, `buf.h`/`BSIZE=4096`）

### 系统调用

所有系统调用均在 `kernel/syscall.c` 中实现，该文件是单体文件（约 3000 行）。命名规范：每个系统调用为 `sys_<名称>()`。主要系统调用：`openat`、`read`/`write`/`readv`/`writev`、`fork`/`clone`/`execve`/`wait`/`exit`、`mmap`/`munmap`/`mprotect`/`mremap`、`futex`、socket 调用（`socket`/`bind`/`connect`/`listen`）、`statx`/`fstatat`/`fstat`、`getdents64`、`mount`/`umount`。

### 关键设计模式

- **自旋锁**（`spinlock.c`）通过 `acquire()`/`release()` 保护共享数据结构，在临界区中关闭中断
- **睡眠锁**（`sleeplock.c`）用于需要较长时间持有且允许进程睡眠的锁
- **睡眠/唤醒**（`sleep_on_chan`/`wakeup`）用于在任意地址上实现阻塞同步
- 错误码使用 Linux 风格的负 errno 值（`-ENOENT`、`-ENOMEM` 等）

### 重要约束

- `-DNUMCPU=1` — 仅单核，无需考虑多核同步
- `NPROC=16` — 最多 16 个进程
- `-DDEBUG=0`（默认关闭），通过 `LOG()` 宏控制调试日志输出
- RISC-V 仅使用 OpenSBI（`-bios default`），入口地址为 `0x80200000`（而非 `0x80000000`）
- LoongArch 使用 `-M virt` 机器类型启动 QEMU
- **非必要不测试**：测试耗时 20 分钟以上，仅在用户明确要求或涉及关键路径修改时才运行测试
- **编译输出很长**：只关注最后几行报错部分，不要浪费 token 在完整编译日志上
