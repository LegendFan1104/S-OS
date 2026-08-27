![SuperOS Logo](image/logo.png)

## S-OS队（SuperOS）

**团队成员：**

| 姓名   | 专业   | 学校   |
| ---- | ---- | ---- |
|  包一帆 |   计算机科学与技术   |   武汉大学   |
|  庄廷钊 |   计算机科学与技术   |   武汉大学   |
|  李嘉乐 |   计算机科学与技术   |   武汉大学   |

| 指导老师 | 学校 |
| ---- | ---- |
| 蔡朝晖   | 武汉大学 |


## 项目简介

**S-OS (SuperOS)** 是一个由 **c 语言**编写的**宏内核操作系统**，基于 xv6 教学操作系统和 oskernel2025 智核速启队 sc7 内核进行完善与优化，面向 RISC-V 64 和 LoongArch 64 两种体系结构，覆盖进程与线程管理、虚拟内存管理（Buddy + Slab + 页表 + VMA）、EXT4 文件系统（集成 lwext4）、VirtIO 块设备驱动（MMIO 与 PCI）以及 Linux 风格系统调用接口（进程、内存、文件、定时器、事件、socket 类别），可适配昉-星光2代（vf2）与龙芯 2k1000 开发板。

本项目获得了**2026年操作系统设计赛内核实现赛-OS内核实现赛道三等奖**。

初赛阶段可通过 basic、busybox、libctest、libcbench、lua、部分ltp 等测试用例。决赛一阶段可通过 cagent 全部测试以及 buildstorm 环境检查与最小构建。


## 文档

开发过程中的详细设计文档、架构说明、开发进展、测试体系等见 **[docx/](docx/)。**

## 运行方法

1. 拉取 docker 官方测评镜像
```bash
docker run -it --name sos -v "$(pwd)":/workspace -w /workspace zhouzhouyi/os-contest:20260510 bash
docker start sos
docker exec -it sos bash
```

2. 容器内编译
```bash
make clean && make all
```

3. 运行 qemu 测试

```bash
# riscv测试
qemu-system-riscv64 -machine virt -kernel kernel-rv -m 1G -nographic -smp 1 -bios default \
  -drive file=sdcard-rv.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -no-reboot \
  -device virtio-net-device,netdev=net \
  -netdev user,id=net \
  -rtc base=utc

# loongarch测试
qemu-system-loongarch64 -kernel kernel-la -m 1G -nographic -smp 1 \
  -drive file=sdcard-la.img,if=none,format=raw,id=x0 \
  -device virtio-blk-pci,drive=x0 \
  -no-reboot \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0 \
  -rtc base=utc
```

4. 真实物理版测试

具体方法见 **[下板文档](docx/下板文档.md)。**

```bash
# riscv 板载
make clean && make board-rv-bin SDMMC_DRIVER=1
# loongarch 板载
make clean && make board-bin SATA_DRIVER=1
```
