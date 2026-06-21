#!/bin/sh
set -eu

# In the current Docker setup the repo root is mounted at /workspace directly.
# /workspace/S-OS is a stale path from an older workflow and should not be used.

IMG_SRC="${1:-/workspace/sdcard-rv.img}"
IMG_TMP="/tmp/rv-test.img"
FSCK_LOG="/tmp/rv-test.fsck"

cp "$IMG_SRC" "$IMG_TMP"
e2fsck -fy "$IMG_TMP" >"$FSCK_LOG" 2>&1 || true

exec qemu-system-riscv64 \
  -machine virt \
  -kernel /workspace/kernel-rv \
  -m 1G \
  -nographic \
  -smp 1 \
  -bios default \
  -drive file="$IMG_TMP",if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -no-reboot \
  -device virtio-net-device,netdev=net \
  -netdev user,id=net \
  -rtc base=utc
