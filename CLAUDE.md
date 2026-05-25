# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 运行环境

- 代码存储在**宿主机**，编译工具链和 QEMU 运行在 **Docker 容器**中，容器名为 `sos1`。
- 执行命令时，建议先进入容器的交互式 shell，再执行后续命令：

```bash
docker exec -it sos1 bash
```

- 进入交互式环境后，`make`、`qemu-*` 等命令可直接执行，无需再加 `docker exec sos1` 前缀。
- 绝对不要直接在宿主机上运行 `make` 或交叉编译工具链。

## 构建命令

```bash
# 全量构建（先 RISC-V，后 LoongArch，包含内核 + 磁盘镜像）
make all

# 单架构构建
make build-release-riscv       # RISC-V release
make build-release-loongarch   # LoongArch release
make build-debug-riscv         # RISC-V debug（带 GDB 符号）

# 单独构建磁盘镜像（将 user/bin/ 打包为 ext4）
make make-image                # RISC-V → disk.img
make make-image-la             # LoongArch → disk-la.img

# 清理所有构建产物
make clean
```

构建产物：`kernel-rv`、`kernel-la`（内核文件），`disk.img`、`disk-la.img`（ext4 根文件系统镜像）。

## QEMU 测试命令

**测试耗时约 30 分钟，非必要不运行。** 仅在用户明确要求或完成关键底层修改后才执行测试。

### RISC-V 测试

```bash
qemu-system-riscv64 -machine virt -bios default -kernel kernel-rv \
  -m 1G -smp 1 -nographic \
  -drive file=basic/sdcard-rv.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -rtc base=utc \
  -drive file=disk.img,if=none,format=raw,id=x1 \
  -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
```

- `virtio-mmio-bus.0`：挂载 `basic/sdcard-rv.img`（测评用 sdcard 镜像）
- `virtio-mmio-bus.1`：挂载 `disk.img`（用户程序数据盘）

### LoongArch 测试

```bash
qemu-system-loongarch64 -kernel kernel-la -m 1G -nographic -smp 1 \
  -drive file=basic/sdcard-la.img,if=none,format=raw,id=x0 \
  -device virtio-blk-pci,drive=x0 -no-reboot \
  -device virtio-net-pci,netdev=net0 -netdev user,id=net0 \
  -rtc base=utc \
  -drive file=disk-la.img,if=none,format=raw,id=x1 \
  -device virtio-blk-pci,drive=x1
```

- 第一个 virtio-blk-pci：挂载 `basic/sdcard-la.img`（测评用 sdcard 镜像）
- 第二个 virtio-blk-pci：挂载 `disk-la.img`（用户程序数据盘）
- `virtio-net-pci`：提供网络支持

### 简化启动（Makefile 封装）

Makefile 中有封装好的 QEMU 启动目标，但内部调用的是 `scripts/qemu.sh` 和 `scripts/qemu-loongarch.sh`，使用的参数可能与上述完整测试命令不同，仅用于快速开发调试：

```bash
make qemu-riscv       # 快速启动 RISC-V（不跑完整测评）
make qemu-loongarch   # 快速启动 LoongArch
make qemu-gdb-riscv   # Debug 模式 + GDB server
```

## 架构概览

**SOS** 是一个支持 **RISC-V**（RV64）和 **LoongArch**（LA64）双架构的操作系统内核。通过编译时宏和目录隔离实现代码复用：

- `#ifdef RISCV` / `#ifdef LOONGARCH` 区分架构相关代码路径
- 架构专有文件放在 `riscv/` 或 `loongarch/` 子目录中，CMake 根据架构自动选择编译
- 平台无关逻辑（调度器、VFS、EXT4、伙伴分配器）直接共用

| 组件 | RISC-V | LoongArch |
|------|--------|-----------|
| 页表 | Sv39（三级） | LA64（四级）+ DMW 直接映射窗口 |
| 中断控制器 | PLIC | LS7A APIC + ExtIOI |
| 设备访问 | MMIO 直接映射 | PCI 枚举 + DMW 映射 |
| 异常入口 | `trampoline.S` | `uservec.S` / `kernelvec.S` |
| 编译器 | `riscv64-unknown-elf-gcc` | `loongarch64-linux-gnu-gcc` |
| 链接脚本 | `kernel/linker/riscv/` | `kernel/linker/loongarch/` |

### 关键子系统

- **内存管理**：伙伴分配器（`kernel/mem/buddysystem.c`）→ 页分配器（`kalloc.c`）→ 虚拟内存（`vm.c`），支持 lazy allocation 和 mmap 缺页处理
- **进程管理**：64 槽 `proc[]` 表，简单轮转调度，支持 `fork`/`execve`/`clone`，进程状态流：UNUSED → USED → RUNNABLE ↔ RUNNING ↔ SLEEPING → ZOMBIE
- **文件系统**：VFS 抽象层（`kernel/fs/vfs/`）+ 嵌入式 lwext4 库（`kernel/fs/ext4/lwext4/`），支持常规文件、管道、socket、设备文件，带缓冲区缓存（`bio.c`）
- **系统调用**：100+ Linux 兼容系统调用，路径：`ecall` → `trampoline.S`（uservec）→ `usertrap()` → `syscall()` → `sys_xxx()` → `usertrapret()` → `sret`
- **信号处理**：完整信号机制，支持自定义 handler、sigreturn trampoline、嵌套信号帧链表
- **同步机制**：自旋锁（关中断）、睡眠锁、信号量，核心 sleep/wakeup 等待唤醒机制
- **用户程序**：源码位于 `user/app/` 和 `user/tests/`，链接 `ulib`，编译后打包进 ext4 磁盘镜像

### 用户进程虚拟地址布局

```
0x0                   代码段 & 数据段（ELF 加载）
  ...                 堆（brk 扩展）
USTACK                用户栈（32 页 + guard page）
  ...
SIG_TRAMPOLINE        信号 trampoline 代码
TRAMPOLINE（RISC-V）  异常进出 trampoline
TRAPFRAME             进程异常上下文
KSTACK                内核栈（每进程 6 页 + guard）
```

### 磁盘布局

- **VIRTIO0**（第一个 virtio 块设备）：rootfs（EXT4，512MB），含 BusyBox 和测试程序
- **VIRTIO1**（第二个 virtio 块设备）：辅助数据盘（EXT4）

## 注意事项

- 修改底层代码（中断、页表、寄存器、汇编）时，务必先确认文件属于哪个架构，避免跨架构污染。
- RISC-V 构建包含 `libc/` 头文件，LoongArch 使用工具链自带的 libc（见 CMakeLists.txt:50-52），两者不可混用。
- 用户程序编译后输出到 `bin/app/`，测试用脚本位于 `user/tests/`。
- `basic/` 目录存放预构建的 sdcard 镜像（测评用），不可随意修改。
- `oskernel2025-a20/` 是外部参考实现，不属于 SOS 核心构建系统。
- 修改用户程序后需重新 `make make-image`（或 `make-image-la`）以更新磁盘镜像中的内容。
