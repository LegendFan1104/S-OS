# SOS (Super operating System) 操作系统架构文档


## 项目概述

**SOS** 是一个支持 **RISC-V** 和 **LoongArch** 双 CPU 架构的操作系统内核。项目以类 Unix 设计为目标，实现了完整的多进程、虚拟内存、EXT4 文件系统、信号处理、Socket 通信等机制，能够运行 BusyBox 等用户态程序。

## 整体架构

```
┌──────────────────────────────────────────────────────────────┐
│                        User Space                            │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐                  │
│   │ initcode │  │ BusyBox  │  │  其他App  │                  │
│   └──────────┘  └──────────┘  └──────────┘                  │
├──────────────────────────────────────────────────────────────┤
│                     System Call Interface                     │
│                   (syscall.c / 100+ syscalls)                │
├──────────────────────────────────────────────────────────────┤
│  ┌──────────┐ ┌─────────┐ ┌──────────┐ ┌─────────────────┐  │
│  │  Process │ │ Memory  │ │   VFS    │ │   Device Driver  │  │
│  │   Mgmt   │ │   Mgmt  │ │  Layer   │ │   (virtio/UART)  │  │
│  └──────────┘ └─────────┘ └──────────┘ └─────────────────┘  │
│  ┌──────────┐ ┌─────────┐ ┌──────────┐ ┌─────────────────┐  │
│  │  Signal  │ │   IPC   │ │   EXT4   │ │   Interrupt      │  │
│  │  Handler │ │ Pipe/   │ │  (lwext4)│ │   (PLIC/APIC)    │  │
│  │          │ │ Socket  │ │          │ │                  │  │
│  └──────────┘ └─────────┘ └──────────┘ └─────────────────┘  │
├──────────────────────────────────────────────────────────────┤
│                       Synchronization                         │
│          Spinlock / Sleeplock / Semaphore                     │
├──────────────────────────────────────────────────────────────┤
│              Architecture Abstraction Layer                    │
│        (RISCV vs LOONGARCH via compile-time macros)           │
└──────────────────────────────────────────────────────────────┘
```

---

## 1. 项目目录结构

```
sos/
├── kernel/           # 内核源码
│   ├── boot/         # 启动代码 (entry.S, main.c, initcode.S)
│   ├── driver/       # 设备驱动 (virtio磁盘, PCI)
│   ├── fs/           # 文件系统
│   │   ├── vfs/      # 虚拟文件系统层
│   │   └── ext4/     # EXT4实现 (基于lwext4)
│   ├── lib/          # 基础库 (printf, string, console)
│   ├── linker/       # 链接脚本
│   ├── mem/          # 内存管理 (伙伴分配器, 页表, slab)
│   ├── proc/         # 进程管理, 信号, 管道, socket
│   ├── sys/          # 系统调用入口
│   └── trap/         # 中断/异常处理
├── include/          # 内核头文件 (按子系统组织)
├── include2/         # newlib C库头文件
├── user/             # 用户程序及initcode
├── scripts/          # QEMU启动/GDB调试脚本
├── data/             # 镜像挂载点
├── CMakeLists.txt    # 构建系统
└── Makefile          # 顶层构建入口
```

---

## 2. 双架构支持策略

项目同时支持 **RISC-V** (RV64) 和 **LoongArch** (LA64) 两种指令集架构，通过以下机制实现代码复用与隔离：

- **编译时条件选择**: 使用 `#ifdef RISCV` / `#elif defined(LOONGARCH)` 宏来区分架构相关的代码路径
- **目录级隔离**: 架构专有文件放在 `riscv/` 或 `loongarch/` 子目录中，由 CMake 构建系统自动选择编译
- **共用代码**: 平台无关的逻辑（如调度算法、EXT4 文件系统）直接共用

主要差异点：
| 组件 | RISC-V | LoongArch |
|------|--------|-----------|
| 页表层级 | Sv39 (3级) | LA64 (4级) 带 DMW |
| 中断控制器 | PLIC | LS7A APIC + ExtIOI |
| 设备访问 | MMIO 直接映射 | PCI 枚举 + DMW 映射 |
| 异常入口 | trampoline.S | uservec.S + kernelvec.S |

---

## 3. 启动流程 (Boot)

### 3.1 入口点

- **RISC-V**: `kernel/boot/riscv/entry.S` - 在机器模式下启动，设置栈后跳转到 `start()`
- **LoongArch**: `kernel/boot/loongarch/entry.S` - 在 CSR 寄存器配置后跳转到 `main()`

### 3.2 启动顺序 (`kernel/boot/main.c`)

```
main()
 ├── consoleinit()         # 串口初始化
 ├── printfinit()          # 格式化输出初始化
 ├── kinit()               # 伙伴分配器初始化 (物理内存管理)
 ├── kvminit()             # 创建内核页表 (直接映射)
 ├── kvminithart()         # 开启分页
 ├── procinit()            # 进程表初始化
 ├── trapinit() / trapinithart()  # 中断向量设置
 ├── plicinit() / apic_init()     # 中断控制器初始化
 ├── virtio_disk_init2()   # rootfs 块设备
 ├── virtio_disk_init()    # 数据块设备
 ├── init_fs_table()       # VFS 文件系统表
 ├── binit()               # 磁盘块缓存
 ├── fileinit()            # 文件表
 ├── inodeinit()           # inode 表
 ├── vfs_ext4_init()       # EXT4 文件系统初始化
 ├── userinit()            # 第一个用户进程 (initcode)
 └── scheduler()           # 进入调度循环
```

---

## 4. 内存管理

### 4.1 物理内存布局

**RISC-V (qemu virt)**:
```
0x10000000   UART0 MMIO
0x10001000   VIRTIO0 (rootfs)
0x10002000   VIRTIO1 (data disk)
0x02000000   CLINT (timer)
0x0C000000   PLIC (interrupt controller)
0x80000000   RAM start → kernel → end → allocatable pages → 0x88000000 (PHYSTOP)
```

**LoongArch (qemu virt)**:
```
0x9000000000000000   DMW 窗口基址
0x1fe001e0           UART 基址
0x10000000           LS7A 桥片寄存器
物理 RAM            0x90000000 ~ 0xB0000000 (512MB)
```

### 4.2 伙伴分配器 (Buddy System)

实现于 `kernel/mem/buddysystem.c`，用于管理物理页面分配。

- **核心数据结构**: 线段树式二叉树，每个节点表示一段连续内存
- **节点状态**: `NODE_UNUSED` / `NODE_USED` / `NODE_SPLIT` / `NODE_FULL`
- **分配算法**: 递归查找能满足请求大小的最小可用块，必要时分裂大块
- **释放算法**: 标记节点为未使用，尝试与相邻buddy合并
- **时间复杂度**: O(log N) 分配与释放
- **接口**:
  - `buddyalloc(bs, s)` — 分配 2^ceil(log2(s)) 个连续页
  - `buddyfree(bs, offset)` — 释放从offset开始的页块

### 4.3 页面分配器 (kalloc)

实现于 `kernel/mem/kalloc.c`，在伙伴系统之上提供单页和多页分配：

- `kalloc()` — 分配一页 (4096 字节)，清零后返回
- `kfree(pa)` — 释放单页
- `kmalloc(size)` — 分配多页，按需求舍入到页边界
- `kcalloc(n, size)` — 分配并清零

物理页面通过 `pa_start` (内核可分配区的起始地址) 与伙伴系统的 page number 之间互相转换。

### 4.4 虚拟内存 (vm.c)

#### RISC-V Sv39 页表
- 三级页表，每级 512 项
- 虚拟地址编码: `[38:30]=L2, [29:21]=L1, [20:12]=L0, [11:0]=offset`
- 支持 39 位虚拟地址空间 (512GB)

#### LoongArch LA64 页表
- 四级页表，支持 DMW (Direct Mapping Window) 固定映射
- DMW 窗口: `0x9000000000000000 → 0x0000000000000000` 直接映射

#### 关键函数
| 函数 | 功能 |
|------|------|
| `kvmmake()` | 创建内核直接映射页表 |
| `kvminit()` | 初始化内核页表 |
| `kvminithart()` | 写入 SATP/PGDL 寄存器开启分页 |
| `walk(pagetable, va, alloc)` | 遍历页表，返回 PTE 指针 |
| `mappages()` | 建立 VA→PA 映射 |
| `uvmalloc()` | 为用户进程增长内存 |
| `uvmdealloc()` | 缩减用户内存 |
| `uvmcopy()` | fork 时复制页表 |
| `copyin/copyout()` | 内核↔用户空间数据拷贝 |
| `pagefault_handler()` | 缺页处理 (支持 lazy allocation + mmap) |

### 4.5 用户进程内存布局

```
0x00000000           text & data (ELF 加载)
    ...              heap (brk 扩展)
USTACK               user stack (32 pages, 底部 guard page)
USTACK_TOP           stack top
    ...
SIG_TRAMPOLINE       signal trampoline code
TRAMPOLINE (RISC-V)  trap entry/exit trampoline
TRAPFRAME            进程上下文保存区
KSTACK(0..NPROC-1)  内核栈 (每进程6页 + guard)
```

---

## 5. 进程管理

### 5.1 进程状态机

```
                        ┌──────────┐
                        │  UNUSED  │
                        └────┬─────┘
                             │ allocproc()
                        ┌────▼─────┐
             yield()    │  USED    │
           ┌───────────►│          │◄──────────┐
           │            └────┬─────┘           │
           │                 │ userinit()/     │
           │                 │ fork()/clone()  │
           │            ┌────▼─────┐           │
           │    ┌───────│ RUNNABLE │◄──────┐   │
           │    │       └────┬─────┘       │   │
           │    │            │ scheduler() │   │
           │    │       ┌────▼─────┐       │   │
           │    │       │ RUNNING  │───────┘   │
           │    │       └────┬─────┘  yield()  │
           │    │            │ sleep()         │
           │    │       ┌────▼─────┐       │   │
           │    │       │ SLEEPING │       │   │
           │    │       └────┬─────┘       │   │
           │    │            │ wakeup()    │   │
           │    │            └─────────────┘   │
           │    │            exit()            │
           │    │       ┌────▼─────┐           │
           │    └──────►│ ZOMBIE  │           │
           │            └────┬─────┘           │
           │                 │ wait()/wait4()  │
           │                 │ freeproc()      │
           │            ┌────▼─────┐           │
           └────────────┤  UNUSED  │───────────┘
                        └──────────┘
```

### 5.2 进程控制块 (proc.h)

每个进程由一个 `struct proc` 表示：
- **调度上下文**: `state`, `pid`, `parent`, `chan`, `context`
- **内存**: `pagetable`, `sz`, `kstack`, `trapframe`
- **文件**: `ofile[NOFILE]`, `cwd`, `vma[NVMA]`
- **信号**: `sig`, `block`, `signal`, `sig_frame`
- **安全**: `uid`, `gid`

进程表大小: `NPROC = 64`

### 5.3 调度器 (scheduler)

- 采用**简单的轮转调度** (Round-Robin)
- 遍历 `proc[]` 数组，选择第一个 `RUNNABLE` 进程运行
- 上下文切换通过 `swtch(&old_context, &new_context)` 实现
- 时间片机制: 每 5 个时钟中断触发一次 `yield()`

### 5.4 进程创建机制

| 机制 | 系统调用 | 要点 |
|------|----------|------|
| `fork()` | SYS_fork | 复制父进程页表、文件表、VMA |
| `execve()` | SYS_execve | 解析 ELF，加载新程序镜像，设置栈和 AUX 向量 |
| `clone()` | SYS_clone | 支持 CLONE_VM/CLONE_FS/CLONE_FILES/CLONE_PARENT 标志 |

### 5.5 execve 实现细节

1. 解析 ELF 头，验证魔数
2. 遍历 Program Header，加载 `PT_LOAD` 段到新页表
3. 分配用户栈 (USTACK)，设置栈保护页
4. 构造 AUX 向量 (AT_PHDR, AT_PAGESZ, AT_ENTRY, AT_RANDOM 等)
5. 压入 argc、argv、envp 到用户栈
6. 切换进程页表，跳转到 ELF entry point

---

## 6. 中断与异常处理

### 6.1 RISC-V 异常处理

```
用户态异常 → trampoline.S (uservec)
        ↓
    usertrap()        ← trap.c
        ├── syscall()     # ecall (cause=8)
        ├── devintr()     # 设备中断 (时钟/UART/磁盘)
        └── pagefault_handler()  # 缺页 (cause=13/15)
        ↓
    usertrapret()     → trampoline.S (userret)

内核态异常 → kernelvec.S → kerneltrap()
```

### 6.2 中断控制器

- **RISC-V**: PLIC (Platform-Level Interrupt Controller)
  - `plicinit()` 配置优先级和使能位
  - `plic_claim()` / `plic_complete()` 处理中断

- **LoongArch**: LS7A APIC + ExtIOI
  - `apic_init()` 初始化核内中断控制器
  - `extioi_init()` 配置扩展 I/O 中断控制器
  - 支持 PCIe MSI 中断

---

## 7. 文件系统

### 7.1 虚拟文件系统 (VFS)

VFS 层位于 `kernel/fs/vfs/`，定义了统一的文件系统接口：

```c
struct file_operations {
    struct file* (*dup)(struct file*);
    void (*close)(struct file*);
    int  (*read)(struct file*, uint64 addr, int n);
    int  (*write)(struct file*, uint64 addr, int n);
    int  (*readat)(struct file*, uint64 addr, int n, uint64 off);
    int  (*fstat)(struct file*, uint64 addr);
    int  (*statx)(struct file*, uint64 addr);
    int  (*readable)(struct file*);
    int  (*writable)(struct file*);
};
```

支持的文件类型 (`f_type`):
- `FD_NONE` — 未使用
- `FD_PIPE` — 管道
- `FD_REG` — 普通文件 (EXT4)
- `FD_DEVICE` — 设备文件
- `FD_SOCKET` — Socket
- `FD_SYSFILE` — 系统文件

### 7.2 EXT4 文件系统

基于 **lwext4** 嵌入式 EXT4 库实现，位于 `kernel/fs/ext4/lwext4/`：

- **块设备抽象**: `kernel/fs/ext4/vfs_ext4_blockdev_ext.c` — 通过 virtio 磁盘驱动提供块读写接口
- **VFS 集成**: `kernel/fs/ext4/vfs_ext4_ext.c` — 将 lwext4 接口适配到内核 VFS 层
- **核心模块**:
  - `ext4_super.c` — 超级块管理
  - `ext4_inode.c` — inode 操作
  - `ext4_dir.c` / `ext4_dir_idx.c` — 目录索引
  - `ext4_extent.c` — extent 树
  - `ext4_balloc.c` — 块分配
  - `ext4_ialloc.c` — inode 分配
  - `ext4_journal.c` — 日志
  - `ext4_mkfs.c` — 文件系统创建

### 7.3 缓冲区缓存 (Buffer Cache)

实现于 `kernel/fs/vfs/` (bio.c)，提供磁盘块的 LRU 缓存：
- 缓存大小: `NBUF` 个块 (每块 BSIZE 字节)
- 使用 sleeplock 保护每个 buffer
- 支持 bread/bwrite/brelse 接口

### 7.4 路径解析

```
path → get_absolute_path() → vfs_ext_namei() → inode
                                    ↓
                              EXT4 dir lookup
```

- `get_absolute_path()` 处理相对路径、`.` 和 `..`、多余斜杠
- 最终通过 EXT4 目录查找获取 inode

---

## 8. 设备驱动

### 8.1 VirtIO 块设备驱动

**RISC-V 版本** (`kernel/driver/riscv/virtio_disk.c`):
- 通过 MMIO 寄存器直接访问
- 使用 VirtIO 传统模式 (legacy interface)
- 三个 descriptor 链: header → data → status
- 磁盘操作通过 `virtio_disk_rw()` / `virtio_disk_rw2()` (两个磁盘) 实现
- 中断处理: `virtio_disk_intr()` / `virtio_disk_intr2()`

**LoongArch 版本** (`kernel/driver/loongarch/virtio_disk.c`):
- 通过 PCI 枚举发现 virtio 设备
- 使用 `virtio_pci.c` 进行 PCI 配置空间访问
- `virtio_ring.c` 实现 vring 操作

### 8.2 磁盘布局

- **VIRTIO0** (第一个 virtio 块设备): rootfs，包含 EXT4 root 文件系统
- **VIRTIO1** (第二个 virtio 块设备): 数据磁盘，也格式化为 EXT4

---

## 9. 同步机制

| 机制 | 文件 | 特点 |
|------|------|------|
| **Spinlock** | `kernel/proc/spinlock.c` | 自旋锁，关中断；`acquire()`/`release()`/`push_off()`/`pop_off()` |
| **Sleeplock** | `kernel/proc/sleeplock.c` | 睡眠锁，持有期间允许被调度；`acquiresleep()`/`releasesleep()` |
| **Semaphore** | `kernel/proc/semaphore.c` | 信号量，基于 sleep/wakeup 实现 |

`sleep(chan, lk)` / `wakeup(chan)` 是核心的等待/唤醒机制，用于实现锁和进程间同步。

---

## 10. 系统调用

### 10.1 系统调用路径

```
用户程序: ecall
    ↓
trampoline.S → usertrap() → syscall()
    ↓
syscalls[num]()   ← 根据 a7 寄存器获取调用号
    ↓
sys_xxx()         ← 具体系统调用实现
    ↓
返回值写入 trapframe->a0
    ↓
usertrapret() → sret → 用户程序
```

### 10.2 已实现的系统调用 (100+)

| 类别 | 系统调用 |
|------|----------|
| **进程** | fork, execve, exit, exit_group, wait, wait4, clone, getpid, getppid, gettid, getuid, geteuid, getgid, getegid, getpgid, setpgid, setuid, setgid, sched_yield, nanosleep, clock_nanosleep, set_tid_address |
| **文件** | openat, read, write, readv, writev, close, dup, dup3, lseek, fstat, fstatat, statx, getcwd, chdir, getdents64, unlinkat, linkat, mkdirat, mknod, faccessat, fchmodat, symlinkat, readlinkat, renameat2, utimensat, ftruncate, pread64, splice, sendfile, copy_file_range, fcntl, ioctl |
| **内存** | brk, mmap, munmap, mremap, madvise, mprotect |
| **信号** | rt_sigaction, rt_sigprocmask, rt_sigtimedwait, kill, tkill, tgkill, kill_signal |
| **IPC** | pipe2, socket, ppoll, futex |
| **时间** | gettimeofday, times, clock_gettime |
| **系统** | uname, sysinfo, syslog, shutdown, getrandom, mount, umount2, prlimit64 |
| **同步** | set_robust_list |

---

## 11. 信号处理

### 11.1 信号生命周期

```
发送端                          接收端
kill/tkill/tgkill  ──────────→  proc->signal = signum
                                    ↓
                            usertrap() 返回前
                                    ↓
                            handle_signal()
                                ├── SIG_DFL: default_handle()
                                └── 自定义handler:
                                    保存 trapframe → signal_frame 链表
                                    修改 trapframe 跳转到 handler
                                    ra = sig_trampoline
                                        ↓
                                    用户态 handler 执行完毕
                                        ↓
                                    sigreturn (rt_sigreturn syscall)
                                        ↓
                                    sig_return(): 恢复 trapframe
```

### 11.2 信号帧

- `struct signal_frame` 保存被中断进程的完整上下文 (trapframe + 信号掩码)
- 通过链表组织，支持信号嵌套
- sig_trampoline 代码映射在用户空间高地址 (SIG_TRAMPOLINE)

---

## 12. 管道 (Pipe)

实现于 `kernel/proc/pipe.c`：
- 环形缓冲区，大小 1024 字节
- 基于 spinlock 保护的读写
- 读写阻塞语义 (无数据时读阻塞，满时写阻塞)，通过 sleep/wakeup 实现
- 支持内核态直接读写 (`pipewrite_kernel` / `piperead_kernel`)

---

## 13. mmap 支持

通过 `struct vm_area` (VMA) 实现内存映射：
- 每个进程最多 16 个 VMA 区域 (`NVMA = 16`)
- 支持 MAP_SHARED / MAP_PRIVATE
- 支持 PROT_READ / PROT_WRITE / PROT_EXEC 权限
- 缺页时 `pagefault_handler()` 从映射文件读取数据填充物理页
- 进程退出时将 MAP_SHARED + PROT_WRITE 区域写回文件

---

## 14. VMA 内存区域

每个进程维护 VMA 列表来跟踪 mmap 映射：

```c
struct vm_area {
    int used;           // 是否使用中
    uint64 addr;        // 映射起始虚拟地址
    int len;            // 映射长度
    int prot;           // 权限位 (PROT_READ/WRITE/EXEC)
    int flags;          // MAP_SHARED / MAP_PRIVATE
    int vfd;            // 对应文件描述符
    struct file* vfile; // 映射文件指针
    int offset;         // 文件内偏移
};
```

---

## 15. 构建系统

- **CMake** + **Makefile** 双层构建
- 通过 `-DRISCV=ON` 或 `-DLOONGARCH=ON` 选择目标架构
- `make qemu-riscv` → 编译 RISC-V 内核 → 创建 EXT4 磁盘镜像 → 启动 QEMU
- `make qemu-loongarch` → 编译 LoongArch 内核 → 创建 EXT4 磁盘镜像 → 启动 QEMU
- Docker 容器隔离编译环境 (`os_workspace` 容器)

### 镜像构建流程

```
user/bin/  →  mkfs.ext4  →  image/fs.img  →  disk.img (拷贝)
             (512MB)           (EXT4)
```

---

## 16. 关键技术特性总结

| 特性 | 实现状态 |
|------|----------|
| 双架构支持 (RISC-V + LoongArch) | 完整 |
| 多进程 (fork/execve/clone) | 完整 |
| 伙伴系统物理内存分配 | 完整 |
| 虚拟内存 (分页/DMW) | 完整 |
| 缺页处理 (lazy allocation + mmap) | 完整 |
| EXT4 文件系统 (读写/日志) | 完整 |
| VFS 抽象层 | 完整 |
| 磁盘块缓存 | 完整 |
| 管道 (Pipe) | 完整 |
| 信号处理 (31+ 信号/自定义handler) | 完整 |
| Socket 通信 | 支持 |
| Futex 同步 | 支持 |
| 100+ Linux 兼容系统调用 | 完整 |
| BusyBox 兼容 | 完整 |
| PLIC/APIC 中断管理 | 完整 |
| VirtIO 块设备驱动 | 完整 |
| Spinlock / Sleeplock / Semaphore | 完整 |

---

## 17. 参考信息

- 项目基于 xv6-riscv 操作系统教学内核的设计理念扩展而来
- EXT4 支持基于 lwext4 轻量级嵌入式 EXT4 库
- LoongArch 参考龙芯 LS7A 桥片手册进行设备驱动开发
- 目标运行环境: QEMU 系统模拟器 (virt 机器, riscv64 / loongarch64)
