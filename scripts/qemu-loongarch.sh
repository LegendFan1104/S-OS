
QEMU="qemu-system-loongarch64"

QEMU_ARGS="-kernel kernel-la -m 1G -nographic -smp 1 -drive file=basic/sdcard-la.img,if=none,format=raw,id=x0  \
                -device virtio-blk-pci,drive=x0 -no-reboot  -device virtio-net-pci,netdev=net0 \
                -netdev user,id=net0,hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555  \
                -drive file=disk-la.img,if=none,format=raw,id=x1 -device virtio-blk-pci,drive=x1 \
                -rtc base=utc"

echo "QEMU $QEMU_ARGS"
$QEMU $QEMU_ARGS
