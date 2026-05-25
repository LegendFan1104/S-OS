  支持 LTP 测试的更改摘要

  1. PT_INTERP / 动态链接器支持 (kernel/proc/exec.c, include/mem/memlayout.h)

  - 修复了死代码 bug：RISC-V 的 PH 循环在 if(ph.type != ELF_PROG_LOAD) continue; 之后有一个无法到达的 if(ph.type ==
  ELF_PROG_INTERP) 检查
  - 添加了 PT_INTERP 路径读取：从 ELF 段正确读取解释器路径（例如 /lib/ld-linux-riscv64-lp64d.so.1）
  - 添加了 resolve_interp()：多级解释器解析——尝试精确路径，然后
  /mnt/glibc/lib/<basename>，/mnt/musl/lib/<basename>，以及 musl 的 libc.so（作为 ldso 使用）
  - 添加了 uvmmap_range()：在任意地址映射页面以加载解释器（因为 uvmalloc 需要连续地址）
  - 正确设置了 AT_BASE/AT_ENTRY aux 向量，并将 trapframe->epc 设置为解释器入口点
  - 定义了 INTERP_BASE：在 memlayout.h 中为 RISC-V 和 LoongArch 定义

  2. 设备文件修复 (kernel/fs/vfs/file.c, kernel/sys/sysfile.c)

  - /dev/null：读取正确返回 0 (EOF)；写入消耗并返回 n
  - /dev/zero：读取正确用零填充用户缓冲区并返回 n；写入消耗数据
  - /dev/urandom 和 /dev/random：使用 PCG 风格的 PRNG 实现（带真实输出）；接受写入
  - /dev/cpu_dma_latency：保持现有行为
  - 修复了 sys_readv：/dev/urandom 现在使用 PRNG 而不是未初始化的 kmalloc 内存
  - 设备节点的 stat/fstat/statx 现在返回合理的值（字符设备 0x2000）

  3. 关键系统调用（25 个新增）

  - 进程：prctl（15+ 个操作）、getrlimit、setrlimit、getrusage、getpriority、setpriority、umask、personality
  - 文件系统：statfs、fstatfs、sync、fsync、fdatasync、fchownat、mknodat
  - 信号：rt_sigpending、rt_sigsuspend、sigaltstack
  - 凭证：setresuid、getresuid、setresgid、getresgid、getgroups、setgroups
  - proc 结构：添加了 umask、pending（信号集）、altstack 字段

  4. LTP 测试运行器 (user/app/init-riscv.c)

  - run_ltp_tests()：扫描 musl/ltp/testcases/bin/ 和 glibc/ltp/testcases/bin/ 中的 LTP 二进制文件，直接分叉执行每个文件
  - run_ltp_case()：使用轮询 wait4(WNOHANG) 实现每个测试 60 秒超时；通过 kill() 终止超时的测试
  - 黑名单：约 80 个前缀条目，涵盖网络、ptrace、高级 IPC、inotify、epoll、定时器、cgroups、xattrs 等——所有这些在 SOS
  中都不支持
  - 结果输出：为解析兼容性输出 RUN LTP CASE / END LTP CASE / FAIL LTP CASE / SKIP LTP CASE
  - 修改了 auto_run_tests：跳过 ltp_testcode.sh 并直接运行 LTP 二进制文件
  - 构建：完整内核编译成功