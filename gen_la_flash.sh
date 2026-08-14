#!/bin/bash
# 2K1000 SATA 镜像分块 + 生成 U-Boot 烧录脚本
# 用法: ./gen_la_flash.sh [块大小MiB]    默认 64，范围 16-128
# 产物:
#   la-flash/chunk-000.bin ... chunk-NNN.bin   分块
#   la-flash/flash.cmd                         逐条命令（可粘贴执行）
#   la-flash/flash.scr                         mkimage 打包（可选）
set -e
cd "$(dirname "$0")"

IMG=sdcard-la.img
CHUNK_MIB=${1:-64}
if [ "$CHUNK_MIB" -lt 16 ] || [ "$CHUNK_MIB" -gt 128 ]; then
    echo "块大小建议 16-128 MiB"
    exit 1
fi
CHUNK=$((CHUNK_MIB * 1024 * 1024))

[ -f "$IMG" ] || { echo "缺少 $IMG"; exit 1; }
SIZE=$(stat -c %s "$IMG")
NCHUNK=$(( (SIZE + CHUNK - 1) / CHUNK ))
echo "==> $IMG: $SIZE bytes, 每块 ${CHUNK_MIB}MiB, 共 $NCHUNK 块"

rm -rf la-flash
mkdir -p la-flash/chunks

# 1. 分块并补 .bin 后缀
split -b "$CHUNK" -d -a 3 "$IMG" la-flash/chunks/chunk-
cd la-flash/chunks
for f in chunk-*; do mv -- "$f" "$f.bin"; done
cd ..

# 2. 生成 flash.cmd：偏移 = i*块字节/512，count = 块扇区数（最后一块可能不满）
#    注意：U-Boot 的 scsi write 用 hextoul 解析 blk/cnt（十六进制），
#    所以这里必须写 0x 前缀的十六进制数，否则 131072 会被当成 0x131072=1249394。
python3 - "$SIZE" "$CHUNK" <<'EOF'
import sys
size, chunk = int(sys.argv[1]), int(sys.argv[2])
with open("flash.cmd", "w") as f:
    f.write("scsi reset\nscsi info\n")
    for i in range(0, size, chunk):
        name = f"chunk-{i // chunk:03d}.bin"
        blk = i // 512
        cnt = min(chunk, size - i) // 512
        f.write(f"tftpboot 0x9000000008000000 {name}\n")
        f.write(f"scsi write 0x9000000008000000 0x{blk:x} 0x{cnt:x}\n")
EOF

# 3. 打包 flash.scr（source 不校验架构；优先 loongarch，不支持就用 riscv）
if command -v mkimage >/dev/null 2>&1; then
    if mkimage -A loongarch -T script -C none -n la-flash \
               -d flash.cmd flash.scr >/dev/null 2>&1; then
        echo "==> flash.scr (loongarch)"
    else
        mkimage -A riscv -T script -C none -n la-flash \
                -d flash.cmd flash.scr >/dev/null
        echo "==> flash.scr (riscv 标记，source 不校验架构)"
    fi
else
    echo "==> 未找到 mkimage，跳过打包；可把 flash.cmd 粘贴进 U-Boot 执行"
fi

# 4. 校验
CNT=$(ls chunks | wc -l)
echo "==> 分块数 $CNT / 预期 $NCHUNK"
echo "==> 总扇区 $((SIZE / 512))，磁盘需 ≥ $(( (SIZE + 1073741823) / 1073741824 ))GiB"
echo "==> flash.cmd 开头："
head -6 flash.cmd
ls -l flash.cmd flash.scr 2>/dev/null || true
