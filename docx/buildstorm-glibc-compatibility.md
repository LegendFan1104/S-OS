# BuildStorm glibc 兼容性与修复记录

## 1. 范围与结论

本次工作针对 RISC-V、glibc 用户态、单核配置，使用评测实际镜像
`sdcard-rv.img`。测试入口为 `user/riscv/user.c` 中的 `run_submit()`，执行
顺序为：

1. `run_final_scripts()`，其中先完成 cagent 测试；
2. `run_buildstorm()`，执行 BuildStorm 脚本；
3. 关机。

当前已通过本题 2.1 的两个测试点，实际串口输出为：

```text
BUILDSTORM_TOOLCHAIN ok
BUILDSTORM_MINIBUILD ok
```

这证明 RISC-V glibc 动态链接、`rustc/cargo --version`、cargo 创建项目、
编译并运行 Hello World 的完整链路已经打通。本次没有宣称 2.2 的完整
ArceOS 编译成功；使用 180 秒诊断超时时，测试已进入 `tg-xtask` 预编阶段，
之后由诊断超时结束。

## 2. 问题定位与根因

### 2.1 TCGETS 的 termios ABI 大小错误

最初所有动态链接工具都出现：

```text
*** stack smashing detected ***: terminated
Aborted
```

内核的 `TCGETS` 实现把 Linux 内核态 termios 定义成了 32 个控制字符，
结构体大小为 52 字节。Linux RISC-V 内核 ABI 的 `NCCS` 实际为 19，正确
大小是 36 字节。glibc 的 `tcgetattr()` 将栈上的 80 字节区域传给 ioctl，
栈保护值位于约 40 字节处；内核多写出的数据覆盖了该保护值，所以触发了
glibc 的栈保护，而不是 rustc 自身错误。

修复位置：`kernel/syscall.c` 的 `linux_termios_local`，将 `c_cc` 改为
19 字节，并继续通过 `copyout()` 返回 36 字节的内核 ABI 数据。

### 2.2 openat 没有跟随最终符号链接

镜像中的：

```text
/usr/lib/riscv64-linux-gnu/libatomic.so.1
```

是指向 `libatomic.so.1.2.0` 的符号链接。原来的 `openat` 直接把符号链接
文件内容交给动态加载器，加载器读到的是目标路径文本，因此报：

```text
invalid ELF header
```

修复位置：`kernel/syscall.c` 的 `resolve_open_symlinks()`。`sys_openat()`
现在在真正打开普通文件前解析最多 8 层绝对或相对符号链接，并把解析后的
路径交给 ext4 打开。这样动态加载器取得的是真实 ELF 文件。

### 2.3 `/dev/null` 写返回值错误

BuildStorm 脚本会把 `mount -t proc/sysfs/devtmpfs` 的错误输出重定向到
`/dev/null`。内核虽然暂不支持这些文件系统类型，但脚本会忽略 mount 的
错误；原来的 `devnullwrite()` 对非空写入返回 0。glibc 将 0 视为短写并
反复重试，导致脚本卡住。

修复位置：`kernel/console.c` 的 `devnullwrite()`，按照 Linux `/dev/null`
语义直接返回请求长度 `n`，表示所有数据已消费。mount 的“不支持文件系统
类型”提示仍然存在，但不会再阻塞后续测试。

### 2.4 buddy 空闲链表与元数据不一致

修复上述问题后，测试在 rustc 退出阶段触发：

```text
panic:[list.c] is_interior (elem)
```

诊断地址对应 `buddy_node_t.elem`，不是进程线程队列。伙伴系统原先只依据
位图、块地址和阶数判断伙伴可合并，没有确认该节点仍然挂在对应的空闲链表
中。块在前一次合并中被摘除后，节点的 `prev/next` 可能已被清零，再次执行
`list_remove()` 就触发断言。

修复位置：`kernel/pmem.c`。

- 增加 `buddy_elem_is_linked()`，要求前后链接互相指回当前节点；
- 只有链表关系完整时才执行 `list_remove()`；
- 对“位图显示空闲、节点明确为 null/null、但已经脱链”的情况重新插入
  对应阶的空闲链表，避免继续破坏链表；
- 对其它异常链接停止本次合并，避免内核继续解引用不可信指针。

## 3. 配套兼容性修改

为使真实 glibc 工具链能够持续运行，同时保留了以下必要修复：

- `sys_execve()` 保留调用者的环境变量，BuildStorm 使用镜像中的
  `/usr/lib/riscv64-linux-gnu`、`/usr/lib`、`/lib` 动态库搜索路径；
- RISC-V ELF 装载、`brk`、`mmap/MAP_FIXED`、`munmap`、VMA 拆分和
  `mprotect` 的页边界处理按 Linux 用户态行为修正；
- 叶级 PTE 显式设置访问/脏位，避免依赖可选 Svadu 硬件更新 A/D 位；
- 增加 glibc 会使用的 `sigaltstack`、`riscv_hwprobe` 等系统调用兼容路径；
- `writev`、`stat/fstat`、`/proc/self/maps` 等接口补齐参数检查和 RISC-V
  ABI 数据布局。

## 4. 复现步骤

所有命令在 Docker 容器 `sos2026` 中执行：

```sh
docker exec sos2026 bash -lc 'make clean && make all'
docker exec sos2026 bash -lc 'timeout 180s qemu-system-riscv64 -machine virt -kernel kernel-rv -m 1G -nographic -smp 1 -bios default -drive file=sdcard-rv.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 -no-reboot -device virtio-net-device,netdev=net -netdev user,id=net -rtc base=utc > /tmp/buildstorm.log 2>&1'
```

只查看关键结果时，可将串口输出重定向后执行：

```sh
grep -E 'OS COMP TEST GROUP|BUILDSTORM_TOOLCHAIN|BUILDSTORM_MINIBUILD|BUILDSTORM_RESULT|panic|stack smashing|invalid ELF' /tmp/buildstorm.log
```

本次验证中，cagent 先完整结束，随后 BuildStorm 输出工具链版本，接着
输出 `BUILDSTORM_TOOLCHAIN ok` 和 `BUILDSTORM_MINIBUILD ok`。

## 5. AI 使用说明

AI 辅助过程包括：阅读实际测试入口和镜像内脚本、审查 ELF 装载和系统调用
ABI、使用受限串口日志定位异常、修改内核并在 `sos2026` 容器中复现。定位
过程中没有修改 guest 时钟、`/proc/uptime`、测试判定逻辑或磁盘镜像内容。
所有结论均可用本节命令重新验证。
