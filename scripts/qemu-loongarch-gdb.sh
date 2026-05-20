#!/bin/sh

# Define the QEMU command, GDBPORT, and QEMUOPTS
QEMU="qemu-system-loongarch64"
GDBPORT=$(expr `id -u` % 5000 + 25000)
QEMUOPTS="-machine virt -kernel kernel-la -m 1G -smp 1 -nographic -drive file=basic/sdcard-rv.img,if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0"
# Check if the -gdb option is available in the QEMU help documentation
if $QEMU -help | grep -q '^-gdb'; then
    QEMUGDB="-gdb tcp::$GDBPORT"
else
    QEMUGDB="-s -p $GDBPORT"
fi

sed "s/:1234/:$GDBPORT/" < .gdbinit.tmpl-riscv > .gdbinit
echo "start gdb"
$QEMU $QEMUOPTS -S $QEMUGDB
