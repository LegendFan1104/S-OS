#!/bin/sh

# Define the QEMU command, GDBPORT, and QEMUOPTS
QEMU="qemu-system-riscv64"
GDBPORT=$(expr `id -u` % 5000 + 25000)
QEMUOPTS="-machine virt -bios default -kernel bin/kernel-riscv -m 1G -smp 1 -nographic\
            -drive file=basic/sdcard-rv.img,if=none,format=raw,id=x0 \
            -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
            -rtc base=utc \
            -drive file=disk.img,if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
"
# Check if the -gdb option is available in the QEMU help documentation
if $QEMU -help | grep -q '^-gdb'; then
    QEMUGDB="-gdb tcp::$GDBPORT"
else
    QEMUGDB="-s -p $GDBPORT"
fi

sed "s/:1234/:$GDBPORT/" < .gdbinit.tmpl-riscv > .gdbinit
echo "start gdb"
$QEMU $QEMUOPTS -S $QEMUGDB
