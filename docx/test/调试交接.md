# LoongArch BuildStorm 前 20 分修复 - 调试交接文档

> 本文档记录了截至 2026-08-03 的所有修复与排查结论，供后续对话继续完成
> "LoongArch 跑通 BuildStorm 前 20 分" 任务时使用。旧日志均已作废，以此文档为准。

## 1. 任务与标准流程

目标：`BUILDSTORM_TOOLCHAIN ok`（8 分）+ `BUILDSTORM_MINIBUILD ok`（12 分），且不能破坏已通过的 RISC-V。

测试用例与评测代码在宿主机 `/home/testsuits-for-oskernel`，关键脚本
`/home/testsuits-for-oskernel/scripts/buildstorm_testcode.sh`（judge 解析 `BUILDSTORM_* ok|fail` 行）。
该脚本在 guest 内执行：先跑 `rustc --version && cargo --version`（8 分），再 `cargo new /tmp/minibuild && cargo build` 并运行（12 分），
最后是可选的 `BUILDSTORM_COMPILE`（40+120 分，本次不涉及）。

标准构建与测试（全部在 docker 容器 `sos2026` 内，工作区容器内路径 `/workspace`，宿主机 `/home/oskernel2026-sos`，双向同步）：

```sh
docker exec sos2026 bash -lc "cd /workspace && pkill -9 qemu-system 2>/dev/null; make clean >/dev/null 2>&1 && make all"
docker exec sos2026 bash -lc "cd /workspace && timeout 200 qemu-system-loongarch64 -kernel kernel-la -m 1G -nographic -smp 1 -drive file=sdcard-la.img,if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0 -no-reboot -device virtio-net-pci,netdev=net0 -netdev user,id=net0 -rtc base=utc > /tmp/run.log 2>&1"
```

一次完整运行约 70~80 秒（跑完 buildstorm 测试后自动关机）。镜像内容可挂在宿主机：`/tmp/mnt-la`（只读挂载自 `sdcard-la.img`）。

## 2. 当前状态

- cagent 前 10 项测试：全部通过
- `rustc` 直接运行 OK（`/root/.rustup/toolchains/nightly-2026-05-28-loongarch64-unknown-linux-gnu/bin/rustc --version`）
- `BUILDSTORM_TOOLCHAIN`：**FAIL** —— `/root/.cargo/bin/rustup show` 段错误（SIGSEGV）
- `BUILDSTORM_MINIBUILD`：**FAIL** —— cargo build 异常退出，`/tmp/minibuild/target` 不存在
- RISC-V 未做回归验证（上一次提交前 RISC-V 是通过的；后续对话必须补验）

## 3. 已完成的修复（当前工作区 diff 内容）

以下修改已存在于工作区（`git diff` 共 20 个文件，+1277/-391）：

### 3.1 环境与 Makefile
- `Makefile`：新增 `buildstorm-la: clean; make la TEST_PROFILE=buildstorm-la`；`compile_all` 中 hal 编译加 `QEMU=virt`（修复 ls2k 串口地址导致无输出）
- `user/loongarch/user.c`：移植 RISC-V 的 `buildstorm_compat_script` + `buildstorm_env`，`LD_LIBRARY_PATH` 优先 `/glibc/lib`；临时诊断 `SHELL_ENV_*` / `RUSTC_DIRECT` / `RUSTUP_SHOW` / cargo new 后 busybox ls target
- `kernel/exec.c` 解释器路径：`lib/ld-linux-loongarch-lp64d.so.1`（相对 cwd）→ `/glibc/lib/ld-linux-loongarch-lp64d.so.1`

### 3.2 ELF 加载与用户地址空间
- `kernel/exec.c`：ELF 可写段加 `PTE_D`；`loadaux` 返回类型 `int` → `uint64`（修复栈指针 64→32 位截断）；errno 修正（`-1` → `-ENOENT/-ENOEXEC`）；段加载/解释器映射完善
- `kernel/vmem.c`：`mappages` 对可写页自动加 `PTE_D`
- `include/hal/loongarch/loongarch.h`：新增 `EUEN_LSXEN (1<<1)`、`EUEN_LASXEN (1<<2)`；**MAXUVA `0x80000000` → `0x1000000000`（64GB）**（LoongArch 静态二进制基址 0x120000000，原上限 2GB 导致 mmap 无空间）
- `hsai/hsai_trap.c`：pagefault_handler 处理 PME(ecode 0x4)；已映射页缺页时置 `PTE_D`；`w_csr_euen(FPE_ENABLE | EUEN_LSXEN | EUEN_LASXEN)`

### 3.3 syscall / 诊断
- `kernel/syscall.c`：brk/mmap/pmem 分配失败诊断打印；无条件 `[diag][syscall]` / `[diag][sysret]` 追踪（调试用，**修好后需恢复 FINAL_DEV_DIAG 门控**）
- `kernel/vma.c`：mmap 失败打印、`pmem_free_pages_count` 计数、VMA 拆分/合并/MAP_FIXED 替换逻辑大改

### 3.4 VirtIO DMA（本次新增，**未能解决 rustup 段错误**）
`kernel/driver/loongarch/virtio_disk.c`：
- 新增独立 DMA 缓冲 `dma_buf[BSIZE]`，DMA 不再直接读写 bcache 的 `b->data`，完成后再显式拷贝
- 队列/请求/状态字节通过 DMWIN1 非缓存别名（`LA_UNCACHED` = `(pa & ~0x9000...) | 0x8000...`）访问
- 完成等待由 `while (disk.used_idx == disk.used->id)` 改为**轮询状态字节** `while (*status == 0xff)`（修复首请求 used->id==0 的竞态）
- 关键路径加 `__sync_synchronize()` 屏障
- init 中 `memset` 也走非缓存别名

结论：此修改在 QEMU TCG 下未改变任何行为（崩溃点、s7 乱码值完全一致），说明 QEMU 下 DMA 缓存一致性不是根因。该修改本身正确、可保留（真实硬件上有意义），但**不要指望它解决当前问题**。

### 3.5 原始任务要求逐项核对表（新对话必读）

1. **文件映射与 page fault（VMA 持 `struct file *`，fd 关闭后缺页仍可读）：未实施。**
   `struct vma`（include/kernel/vma.h）只有 `fd`+`f_off`，无独立文件引用。本内核 mmap 是急切读文件，
   fd 关闭不影响已填页面；但 pagefault handler 对文件映射页只填零不读文件。是否需要按提示实施，新对话需结合根因判断。
2. **VirtIO DMA 与缓存一致性：已实施（见 3.4），未解决问题。** QEMU TCG 下无 CPU 缓存，已证明非根因；真实硬件上可保留。
3. **页表和用户地址空间：已实施。** MAXUVA 扩到 64GB、PTE_D/PTE_V/PTE_MAT/PTE_P、PLV3、NX/NR 高位权限（见 3.2）。
4. **ELF 动态链接：已实施。** 解释器路径、段加载/复制/权限、loadaux 64 位、用户寄存器初始化（见 3.2）。
5. **clone3、vfork 与线程生命周期：部分实施。** `sys_clone3` 存在（kernel/syscall.c:4598），CLONE_VM 处理在 1197；
   vfork 恢复父 trapframe、线程退出时文件表处理等未逐项核实。
6. **基础文件系统兼容：部分实施。** fstat 默认字段（st_blksize/st_blocks）已完善；
   **lseek 非普通文件返回 -ESPIPE 未实施**（`vfs_ext4_lseek` 对空 data 仍会 panic，见 kernel/fs/vfs_ext4.c:555）。

## 4. 根因排查过程与结论（重要！）

### 4.1 症状
`rustup show` 段错误，关键信息：
```
[diag][pf-gap] pid=11 addr=0x5410f2c era=0xfeffddb18 badi=0x29c1e061 sz=0xdf3000
  tf: s4=0xfefffd2c0 s5=0x10000 s6=0x380000e0000 s7=0x1583f8 t1=0x10f28 s8=0x5410f28
  s7 page=0x158000 pte=0xa887700f pa=0xa8877000
  s7 words: 0x0 0x380000e0000 0x17a7200002cccc
```
- `era=0xfeffddb18`：ld.so（`/glibc/lib/ld-linux-loongarch-lp64d.so.1`，映射基址 0xfeffd0000）`_dl_relocate_object` 内部
- `s7=0x1583f8`：主程序（rustup，基址 0x10000）DT_RELA 表内偏移 0x1483f8 处（rela 第 55370 项，24 字节对齐）
- 崩溃机制：rela 项内容为乱码（r_info=0x380000e0000，sym 索引 0x380000 非法），ld.so 计算出非法重定位目标 0x5410f28（> p->sz=0xdf3000 且无 VMA 覆盖）→ 写页缺失 → SIGSEGV

### 4.2 决定性证据：rela 页内容 = 磁盘块 0（ext4 超级块）
- rustup 文件在偏移 0x148000-0x148fff 处**本来就全是零**（宿主挂载读 + 原始镜像直读双重确认，rela 表零区域是文件真实内容）
- guest 页 VA 0x158000 实际内容是**磁盘镜像块 0 的字节**：`0x380000e0000 0x17a7200002cccc 0xc889c ... "starry-rootfs\0\0\0/tmp/mnt-la\0" ...`，与原始镜像偏移 0x3e0-0x490 逐字节吻合（128B 前缀在原始镜像中精确定位到 0x3e0，同偏移）
- "starry-rootfs" 是 ext4 超级块卷名（s_volume_name，超级块偏移 0x78 = 镜像偏移 0x478）
- 结论：**读取 rustup 的逻辑块 0x520（文件偏移 0x148000）时，实际拿到了 ext4 块 0（超级块）的内容**

### 4.3 已排除的假设
1. **DMA 缓存一致性**：独立 DMA buffer + DMWIN1 非缓存 + 状态轮询后，乱码完全不变 → QEMU TCG 下无 CPU 缓存，此假设排除
2. **mmap 急切读路径**：该内核 mmap 急切读文件（`readat`），逻辑上只会产生"正确内容或零"，不会产生乱码
3. **pagefault handler 对文件映射页只填零**：确实如此（懒加载页会是零而非乱码），但主程序是 exec 加载（p->sz 覆盖，无 VMA），不走此路径
4. **fork 页复制**：fork 用 uvmcopy 整体复制页表，内容不会丢
5. **exec 段加载写裸物理地址**：`walkaddr` 返回的已是 `0x9000...|PA`（带 DMWIN0 位），写入有效

### 4.4 最可能的根因（待下一对话验证）
**ext4 块映射/缓存问题**：`ext4_fread` → `ext4_blocks_get_direct(bdev, buf, lba=0x520, 1)` → `blockdev_read` → `bread(0, 0x520)`（kernel bcache）→ `la_virtio_disk_rw` 读扇区 0x2900。
扇区换算本身正确（0x520×4096/512 = 0x2900，= 镜像偏移 0x148000）。因此怀疑：
- `ext4_fs_get_inode_dblk_idx`（inode→逻辑块映射，extent/间接块查找）对逻辑块 0x520（块号 1312，超出直接块范围）返回了错误的物理块号（如 0）
- 或 ext4_bcache 元数据缓存项键错/标志错（`ext4_block_get_noread` 先设 `b->lb_id=lba` 再 `ext4_bcache_alloc`，`BC_UPTODATE` 误命中导致跳过真实读取）
- 或 kernel bcache（bio.c）`bget`/LRU 回收在特定情况下返回了错误缓冲块

注意：文件**大部分**块读取正确（ELF 头、程序段、符号表都能用，cagent 也过），只有某些块读错 → 更像"特定逻辑块的映射/缓存项错误"，而非整条 DMA 链路错误。

## 5. 建议的下一步（新对话从这里继续）

1. **验证块映射**：在 `ext4_blocks_get_direct` 或 `blockdev_read` 打印 (lba → 实际 bread 的 blockno)，对 rustup 读偏移 0x148000 处确认 bread 的 blockno 是否为 0x520；再在 `la_virtio_disk_rw` 打印 sector 是否为 0x2900
2. **检查 ext4_bcache**：`kernel/fs/ext4_blockdev.c` 的 `ext4_block_get_noread`/`ext4_bcache_alloc`，重点看 lb_id 键值与 BC_UPTODATE 标志的正确性（怀疑缓存命中逻辑）
3. **检查 extent 查找**：`ext4_fs_get_inode_dblk_idx` 对逻辑块 0x520 的解析（extent 深度/索引），用 debugfs 在宿主机核对 rustup 文件块 0x520 的真实物理块号
4. 修好后移除无条件 `[diag][syscall]`/`[diag][sysret]` 打印，恢复 `FINAL_DEV_DIAG` 门控
5. **必须做 RISC-V 回归**（run-final-rv 或 buildstorm-rv），确认未破坏已通过的 RISC-V 用例

RISC-V 回归方法：`make clean && make TEST_PROFILE=buildstorm-rv all` 生成 `kernel-rv`，
再用 qemu-system-riscv64 + `sdcard-rv.img` 跑 buildstorm 测试，确认 `BUILDSTORM_TOOLCHAIN/MINIBUILD ok`
（RISC-V 具体 qemu 命令参考仓库 README；交接摘要称 RISC-V 在本次修改前已通过前 20 分）。

## 6. 测试/调试辅助信息

- 镜像内容挂载点：宿主机 `/tmp/mnt-la`（sdcard-la.img 只读挂载，已验证与镜像一致）
- rustup 二进制：`/tmp/mnt-la/root/.cargo/bin/rustup`（16,547,096 字节 = 0xFC7D18；DT_RELA=0x3708, RELASZ=0x145F08, RELACOUNT=55278, JMPREL=0x149610）
- ld.so：`/tmp/mnt-la/glibc/lib/ld-linux-loongarch-lp64d.so.1`（第一 LOAD 偏移 0 vaddr 0，基址 0xfeffd0000）
- 磁盘扇区换算：kernel bcache 块 = 4KB（BSIZE=4096），`sector = blockno * 8`；ext4 lg_bsize=4096、ph_bsize=4096、part_offset=0
- 调试开关：`FINAL_DEV_DIAG`（Makefile 传入，当前 0）、`DEBUG`（当前 0）
- 扩展的 pf-gap dump 已留在 `hsai/hsai_trap.c`（打印 s7 页 64 个 8 字节字），便于下次运行直接看 rela 页内容

## 7. 环境注意

- 每次测试前 `pkill -9 qemu-system` 避免镜像锁冲突
- `make clean && make all` 完整构建约 30 秒（容器内）
- docker 命令需要时可能需提权（宿主机 sandbox 限制），当前可直接执行
- 不要用旧日志（用户明确要求重新开始）
