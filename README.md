![SuperOS Logo](image/logo.png)

## S-OS队（SuperOS）

T2026104869910625

## 团队成员

| 姓名   | 专业   | 学校   |
| ---- | ---- | ---- |
|  包一帆 |   计算机科学与技术   |   武汉大学   |
|  庄廷钊 |   计算机科学与技术   |   武汉大学   |
|  李嘉乐 |   计算机科学与技术   |   武汉大学   |


## 项目简介

SOS (SuperOS) 是一个宏内核操作系统，在xv6基础上起步，参考了oskernel2025-智核速启队的架构设计，在此基础上重写锁机制，支持 **RISC-V 64** 和 **LoongArch 64** 双指令集架构。内核以 C 语言编写，兼容部分 Linux ABI，已实现进程/线程调度、完整 EXT4 文件系统（读写）、80+ 系统调用、动态链接器、虚拟 /proc 文件系统等核心功能。

## 部分架构说明

- **进程-线程双层调度模型**：进程（`proc_t`）持有地址空间、文件描述符表、信号处理器等资源；线程（`thread_t`）作为独立调度实体拥有私有的 trapframe 和内核栈。进程内维护 `thread_queue` 环形队列，调度器以线程为粒度做 round-robin，天然支持 `CLONE_THREAD` 语义，线程退出不会错误释放仍在共享的进程资源。
- **睡眠/唤醒的原子语义设计**：`sleep_on_chan(chan, lk)` 在释放调用者锁前先获取进程自旋锁 `p->lock`，再原子完成"释放 lk → 设置 SLEEPING → sched()"的转换，避免了经典的 wakeup 丢失竞态——wakeup 在修改进程状态前同样需要获取 `p->lock`。这一设计用简单的自旋锁嵌套实现了可靠的睡眠/唤醒同步。
- **BusyBox 透明虚拟文件系统**：在内核 VFS 路径中内联拦截 `/proc`、`/proc/self/stat`、`/proc/meminfo`、`/proc/mounts`、`/etc/passwd` 等路径的 `openat`/`fstatat`/`statx`/`getdents64` 调用，按需合成目录项和 stat 信息。不依赖任何用户态守护进程或独立 procfs 层，使 `ps`、`free`、`df`、`hwclock` 等命令零适配运行。
- **exec 内联脚本与重定向引擎**：ELF 加载器中直接集成 shebang 检测、动态链接器路径匹配和 I/O 重定向。`.sh` 文件被 exec 时自动将执行链替换为 `busybox sh <script>`，`>`/`>>` 重定向在 exec 阶段关闭 stdout 并重新打开目标文件，整个过程在 exec 返回用户态前完成，无额外系统调用开销。

## 运行方法

1. 拉取docker官方测评镜像
```bash
docker run -it --name sos -v "$(pwd)":/workspace -w /workspace zhouzhouyi/os-contest:20260510 bash
docker start sos
docker exec -it sos bash
```

2. 容器内编译
```bash
make clean
make all
```
3. 分别运行测试
**RISC-V 测试：**
```bash
qemu-system-riscv64 -machine virt -kernel kernel-rv -m 1G -nographic -smp 1 -bios default \
  -drive file=sdcard-rv.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -no-reboot \
  -device virtio-net-device,netdev=net \
  -netdev user,id=net \
  -rtc base=utc
```

**LoongArch 测试：**
```bash
qemu-system-loongarch64 -kernel kernel-la -m 1G -nographic -smp 1 \
  -drive file=sdcard-la.img,if=none,format=raw,id=x0 \
  -device virtio-blk-pci,drive=x0 \
  -no-reboot \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0 \
  -rtc base=utc
```

## 文档

详细设计文档、架构说明、开发进展、测试体系等见 **[docx/](docx/)** 目录。