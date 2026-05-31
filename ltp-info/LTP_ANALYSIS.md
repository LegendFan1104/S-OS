# SOS Kernel - LTP Syscall Gap Analysis

## 概览

- LTP 版本: 20240524
- 测试二进制总数: 2842 (1949 ELF + 893 shell 脚本)
- `syscalls` 分类: 1799 个测试用例，覆盖 544 个 syscall 模式
- 当前内核实现: ~80 个 syscall
- 预估覆盖率: ~37% (530/1411 test cases in syscalls category)

## 竞赛测试机制

`ltp_testcode.sh` 遍历 `ltp/testcases/bin/` 下所有文件并逐个执行。
测试分类通过 `scenario_groups/default` 定义，包含: syscalls, fs, mm, ipc, cve, sched, math, nptl 等。

## 关键文件位置

- `ltp/runtest/syscalls` - syscall 测试清单 (1799 条)
- `ltp/metadata/ltp.json` - 测试元数据 (超时、依赖、标签)
- `ltp/scenario_groups/default` - 测试分组
- `ltp/testcases/bin/` - 所有测试二进制

## 实现优先级

### P0 - 立即实现 (核心功能，大量测试依赖)

#### 进程/信号
- `futex` 系列: futex_wait, futex_wake, futex_cmp_requeue, futex_waitv (已有基础)
- `waitid` / `waitpid` (wait 已有，需扩展)
- `sigpending`, `rt_sigsuspend`, `rt_sigqueueinfo`, `rt_sigtimedwait`
- `sigaltstack`, `signalfd`, `signalfd4`
- `setsid`, `getsid`, `setpgid`, `getpgid`

#### 文件 I/O
- `fsync`, `fdatasync` - 14 tests
- `fallocate` - 6 tests
- `link`/`linkat`, `symlink`/`symlinkat` - 9 tests
- `epoll_create`/`epoll_ctl`/`epoll_wait`/`epoll_pwait` - 24 tests
- `eventfd`/`eventfd2` - 6 tests
- `preadv`/`pwritev` - 11 tests
- `splice`/`vmsplice`/`tee` - 13 tests
- `flock` - 5 tests
- `sync`/`syncfs` - 4 tests
- `ftruncate`/`truncate` - 3 tests
- `chroot`, `fchdir` - 4 tests

#### 内存
- `madvise` - 10 tests
- `mlock`/`munlock`/`mlockall`/`munlockall` - 11 tests
- `msync`, `mincore` - 8 tests
- `memfd_create` - 4 tests

### P1 - 高优先级 (IPC/权限/定时器/调度)

#### 凭证/权限 (35+ tests)
- `setuid`, `setreuid`, `setresuid`, `setregid`, `setresgid`
- `setgroups`, `getgroups`, `getegid`, `getgid`
- `setfsgid`, `setfsuid`, `getresuid`, `getresgid`
- `capget`, `capset`

#### IPC (26+ tests)
- `msgget`, `msgctl`, `msgsnd`, `msgrcv` (System V 消息队列)
- `semget`, `semctl`, `semop` (System V 信号量)
- `shmdt` (共享内存分离)

#### 定时器/时钟 (15+ tests)
- `timer_create`/`timer_delete`/`timer_settime`/`timer_gettime`
- `timerfd_create`/`timerfd_settime`/`timerfd_gettime`
- `setitimer`/`getitimer`/`alarm`
- `clock_settime`/`clock_getres`

#### 调度器 (14+ tests)
- `sched_getaffinity`/`sched_setaffinity`
- `sched_getparam`/`sched_setparam`
- `sched_getscheduler`/`sched_setscheduler`
- `sched_get_priority_max`/`sched_get_priority_min`
- `nice`, `ioprio_set`/`ioprio_get`

### P2 - 中优先级

- 扩展属性 (xattr): setxattr/getxattr/listxattr/removexattr 系列 (12 syscalls)
- Inotify/Fanotify: 文件系统通知 (6 syscalls, 35 tests)
- 网络扩展: socketpair, sendmsg, recvmsg, getpeername, accept4
- `prctl` - 10 tests
- `madvise` - 10 tests

### P3 - 低优先级 (复杂/特殊用途)

- `ptrace` - 11 tests (调试接口，实现复杂)
- `bpf` - 7 tests
- `io_uring` - 异步 I/O
- `quotactl` - 9 tests (磁盘配额)
- `keyctl`/`add_key`/`request_key` - 19 tests (密钥管理)
- NUMA: `mbind`, `set_mempolicy`, `get_mempolicy`, `migrate_pages`, `move_pages`
- 模块: `init_module`, `delete_module`, `finit_module`
- 挂载新 API: `fsopen`, `fsconfig`, `fsmount`, `fspick`, `mount_setattr`
- `pidfd_open`, `pidfd_send_signal`, `pidfd_getfd`
- `process_vm_readv`, `process_vm_writev`
- `userfaultfd`

## 实现建议

1. **从 P0 开始**，优先实现 fsync/fallocate/epoll/eventfd 等文件 I/O 系统调用
2. **futex 系列已有基础**，补全 futex_waitv 等变体
3. **IPC 相对独立**，msgget/msgctl/msgsnd/msgrcv 和 semget/semctl/semop 可以集中实现
4. **信号相关**需要仔细处理，sigpending/sigsuspend 等涉及进程状态管理
5. **调度器系列**大多是信息查询接口，实现难度低
6. **P3 可以暂时跳过**，除非时间充裕
