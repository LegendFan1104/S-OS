
# Description: Run the kernel in QEMU
QEMU="qemu-system-riscv64"
QEMU_ARGS="-machine virt \
            -bios default \
            -kernel kernel-rv \
            -m 1G -smp 1 -nographic \
            -drive file=basic/sdcard-rv.img,if=none,format=raw,id=x0 \
            -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
            -rtc base=utc \
            -drive file=disk.img,if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
            "
echo "QEMU $QEMU_ARGS"
$QEMU $QEMU_ARGS
