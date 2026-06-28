![SuperOS Logo](image/logo.png)

## S-OS队（SuperOS）

T2026104869910625

## 团队成员

| 姓名   | 专业   | 学校   |
| ---- | ---- | ---- |
|  包一帆 |   计算机科学与技术   |   武汉大学   |
|  庄廷钊 |   计算机科学与技术   |   武汉大学   |
|  李嘉乐 |   计算机科学与技术   |   武汉大学   |

| 指导老师 | 学校 |
| ---- | ---- |
| 蔡朝晖   | 武汉大学 |


## 项目简介

**S-OS (SuperOS)** 是一个宏内核操作系统，基于 xv6 源码和 oskernel2025 智核速启队编写，面向 RISC-V 64 和 LoongArch 64 两种体系结构，覆盖进程与线程管理、虚拟内存管理（Buddy + Slab + 页表 + VMA）、EXT4 文件系统（集成 lwext4）、VirtIO 块设备驱动（MMIO 与 PCI）以及 Linux 风格系统调用接口（进程、内存、文件、定时器、事件、socket 类别），可运行 basic、buxybox、libctest、libcbench、lua、部分ltp 等测试用例。


## 文档

初赛文档、ppt见 **[初赛提交/](初赛提交/)**

受限于仓库大小，初赛答辩视频、PPT 见[这里](https://pan.baidu.com/s/10Kx1rCboyJ2VnzkF5Iv7Kg?pwd=sos3)。

开发过程中的详细设计文档、架构说明、开发进展、测试体系等见 **[docx/](docx/)** 目录。

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
