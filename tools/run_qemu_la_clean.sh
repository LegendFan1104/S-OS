#!/bin/sh
set -eu

# In the current Docker setup the repo root is mounted at /workspace directly.
# /workspace/S-OS is a stale path from an older workflow and should not be used.

IMG_SRC="${1:-/workspace/sdcard-la.img}"
IMG_TMP="/tmp/la-test.img"
FSCK_LOG="/tmp/la-test.fsck"

cp "$IMG_SRC" "$IMG_TMP"
e2fsck -fy "$IMG_TMP" >"$FSCK_LOG" 2>&1 || true

exec qemu-system-loongarch64 \
  -kernel /workspace/kernel-la \
  -m 1G \
  -nographic \
  -smp 1 \
  -drive file="$IMG_TMP",if=none,format=raw,id=x0 \
  -device virtio-blk-pci,drive=x0 \
  -no-reboot \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555 \
  -rtc base=utc
