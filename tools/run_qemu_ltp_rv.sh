#!/bin/sh
set -eu

pkill -f qemu-system-riscv64 2>/dev/null || true
cd /workspace/S-OS
rm -f /tmp/ltp-rv.log

IMAGE_SRC=
for candidate in /workspace/S-OS/sdcard-rv.img /workspace/sdcard-rv.img; do
  if [ -f "$candidate" ]; then
    IMAGE_SRC="$candidate"
    break
  fi
done

if [ -z "$IMAGE_SRC" ]; then
  echo "missing sdcard-rv.img" > /tmp/ltp-rv.log
  exit 1
fi

cp "$IMAGE_SRC" /tmp/sdcard-rv.img

timeout 120s qemu-system-riscv64 \
  -machine virt \
  -kernel build/riscv/kernel-rv \
  -m 1G \
  -nographic \
  -smp 1 \
  -bios default \
  -drive file=/tmp/sdcard-rv.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -no-reboot \
  -device virtio-net-device,netdev=net \
  -netdev user,id=net \
  -rtc base=utc \
  < /dev/null \
  > /tmp/ltp-rv.log 2>&1
