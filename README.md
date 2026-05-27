## S-OS队（SuperOS）

T2026104869910625

## 团队成员

| 姓名   | 专业   | 学校   |
| ---- | ---- | ---- |
|  包一帆 |   计算机科学与技术   |   武汉大学   |
|  庄廷钊 |   计算机科学与技术   |   武汉大学   |
|  李嘉乐 |   计算机科学与技术   |   武汉大学   |

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
  -rtc base=utc \
  -drive file=disk.img,if=none,format=raw,id=x1 \
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
