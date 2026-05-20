#!/bin/bash

objdump="riscv64-unknown-elf-objdump"
# 获取bin/app目录下的所有文件
files=$(ls bin/app)
bins=$(ls -p bin | grep -v /)
cd bin

# 生成objdump-files目录
mkdir -p objdump-files
# 生成objdump-files/asm目录
mkdir -p  objdump-files/asm
# 生成objdump-files/asm/app目录
mkdir -p objdump-files/asm/app
# 生成objdump-files/sym目录
mkdir -p objdump-files/sym
# 生成objdump-files/sym/app目录
mkdir -p objdump-files/sym/app

echo "generating objdump-files in bin/app"
# 遍历所有文件, 生成asm, 以及.sym
for file in $files
do
  $objdump -S app/$file > objdump-files/asm/app/$file.asm
  $objdump -t app/$file | sed -e '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > objdump-files/sym/app/$file.sym
done

echo "generating objdump-files in bin"
# 生成asm, 以及.sym
for bin in $bins
do
  $objdump -S $bin > objdump-files/asm/$bin.asm
  $objdump -t $bin | sed -e '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > objdump-files/sym/$bin.sym
done

echo "generating objdump-files DONE!"