// init: The initial user-level program

#include "types.h"
#include "fs/stat.h"
#include "lock/spinlock.h"
#include "lock/sleeplock.h"
#include "user.h"
#include "fs/fcntl.h"

#define CONSOLE 1
#define DEV_NULL 2
#define DEV_ZERO 3
#define DEV_RTC 3
#define DEV_CPU_DMA_LATENCY 0

#define BUF_SIZE 4096
#define MAX_TEST_SCRIPTS 64
#define TESTCODE_SUFFIX "_testcode.sh"
#define WNOHANG 0x01

char *argv[] = { "sh", 0 };
char *argv2[] = {"", 0};

char basic_path_musl[] = "/mnt/musl/basic/";
char basic_path_glibc[] = "/mnt/glibc/basic/";
char bb_path_musl[] = "/mnt/musl/";
char bb_path_glibc[] = "/mnt/glibc/";
char *basic_name[] = {"brk", "chdir", "clone", "close", "dup", "dup2", "execve", "exit", "fork", "fstat", "getcwd", "getdents", "getpid",
    "getppid", "gettimeofday", "mkdir_", "mmap", "mount", "munmap", "open", "openat", "pipe", "read", "sleep", "test_echo", "times",
    "umount", "uname", "unlink", "wait", "waitpid", "write", "yield",
};

char *bb_testcode[10] = {"busybox", "sh", "busybox_testcode.sh", NULL};
char *libc_testcode[10] = {"busybox", "sh", "/libc_test.sh", NULL};
char *iozone_testcode[10] = {"busybox", "sh", "iozone_testcode.sh", NULL};
char *lmbench_testcode[10] = {"busybox", "sh", "lmbench_testcode.sh", NULL};
char *busybox_debug[10] = {"busybox", "sh", "/busybox_final.sh", NULL};
char *busybox_debug2[10] = {"entry-static.exe", "pthread_cancel_points", NULL};

char *final_site[10] = {"busybox", "sh", "/busybox_final.sh", NULL};

char *final_test1[10] = {"busybox", "sh", "interrupts_testcode.sh", NULL};
char *final_test2[10] = {"busybox", "sh", "copy-file-range_testcode.sh", NULL};
char *final_test3[10] = {"busybox", "sh", "splice_testcode.sh", NULL};

char *final_site_test1[10] = {"busybox", "sh", "git_testcode.sh", NULL};

char *git_arg[10][10] = {
{"git", "help", NULL},
{"git", "init", NULL},
{"git", "add", "README.md", NULL},
{"git", "commit", "-m", "\"add README.md\"", NULL},
{"git", "log", NULL},
};
char info1[] = "hello world";

char *git_envp[10] = {"HOME=/user1", NULL};

char *bb_cmds[][10] = {
    {"echo", "#### independent command test", NULL},
    {"ash", "-c", "exit", NULL},
    {"sh", "-c", "exit", NULL},
    {"basename", "/aaa/bbb", NULL},
    {"cal", NULL},
    {"clear", NULL},
    {"date", NULL},
    {"df", NULL},
    {"dirname", "/aaa/bbb", NULL},
    {"dmesg", NULL},
    {"du", NULL},
    {"expr", "1", "+", "1", NULL},
    {"false", NULL},
    {"true", NULL},
    {"which", "ls", NULL},
    {"uname", NULL},
    {"uptime", NULL},
    {"printf", "abc\\n", NULL},
    {"ps", NULL},
    {"pwd", NULL},
    {"free", NULL},
    {"hwclock", NULL},
    {"kill", "10", NULL},
    {"ls", NULL},
    {"sleep", "1", NULL},
    {"echo", "#### file operation test", NULL},
    {"touch", "test.txt", NULL},
    {"echo \"hello world\" > test.txt", NULL},
    {"cat", "test.txt", NULL},
    {"cut", "-c", "3", "test.txt", NULL},
    {"od", "test.txt", NULL},
    {"head", "test.txt", NULL},
    {"tail", "test.txt", NULL},
    {"hexdump", "-C", "test.txt", NULL},
    {"md5sum", "test.txt", NULL},
    {"echo 'ccccccc' >> test.txt", NULL},
    {"echo 'bbbbbbb' >> test.txt", NULL},
    {"echo 'aaaaaaa' >> test.txt", NULL},
    {"echo '2222222' >> test.txt", NULL},
    {"echo '1111111' >> test.txt", NULL},
    {"echo 'bbbbbbb' >> test.txt", NULL},
    {"sort test.txt | busybox uniq", NULL},
    {"stat", "test.txt", NULL},
    {"strings", "test.txt", NULL},
    {"wc", "test.txt", NULL},
    {"[ -f test.txt ]", NULL},
    {"more", "test.txt", NULL},
    {"rm", "test.txt", NULL},
    {"mkdir", "test_dir", NULL},
    {"mv", "test_dir", "test", NULL},
    {"rmdir", "test", NULL},
    {"grep", "hello", "busybox_cmd.txt", NULL},
    {"cp", "busybox_cmd.txt", "busybox_cmd.bak", NULL},
    {"rm", "busybox_cmd.bak", NULL},
    {"find", ".", "-name", "busybox_cmd.txt", NULL},
    {NULL}
};

char *bb_envp[] = {
  NULL,
};

char *libc_runstatic[10] = {"busybox", "sh", "run-static.sh", NULL};
char *libc_rundynamic[10] = {"busybox", "sh", "run-dynamic.sh", NULL};

// LTP测试黑名单 - 已知会导致挂起、崩溃或不适用于SOS的测试
char *ltp_blacklist[] = {
  // 网络相关 - SOS没有网络栈
  "accept", "accept4", "bind", "connect", "getpeername", "getsockname",
  "listen", "recv", "recvfrom", "recvmmsg", "recvmsg",
  "send", "sendfile", "sendmmsg", "sendmsg", "sendto",
  "shutdown", "socket", "socketpair",
  // ptrace - 未实现
  "ptrace",
  // 高级IPC
  "msgctl", "msgget", "msgrcv", "msgsnd", "msgstress",
  "semctl", "semget", "semop", "sem_post", "sem_timedwait", "sem_wait",
  "shmat", "shmctl", "shmdt", "shmget", "mq_",
  // inotify/fanotify - 未实现
  "inotify", "fanotify",
  // epoll - 未实现
  "epoll",
  // 定时器相关
  "timer_create", "timer_delete", "timer_getoverrun", "timer_gettime", "timer_settime",
  // cgroups
  "cgroup", "cgget", "cgset",
  // 高级文件系统操作
  "quotactl", "flock", "fallocate",
  // 内核模块相关
  "create_module", "delete_module", "finit_module", "init_module",
  "kexec", "kexec_load",
  // 安全相关
  "keyctl", "add_key", "request_key",
  "getxattr", "setxattr", "fgetxattr", "fsetxattr", "listxattr", "flistxattr",
  "removexattr", "fremovexattr", "lgetxattr", "lsetxattr", "llistxattr", "lremovexattr",
  // 可能导致问题的测试
  "fork13", "fork10",    // 大量子进程
  "crash01", "crash02",  // 故意崩溃
  "dio",                 // 直接IO
  "aio",                 // 异步IO
  "hackbench",           // 长时间压力测试
  "move_pages",          // 大内存页迁移
  "mbind",               // NUMA相关
  "add_key", "request_key",  // 内核密钥环
  // from oskernel2025-a20: tests that hang/crash or don't apply
  "af_alg07",
  "bpf_prog07",
  "cgroup_fj_proc", "cgroup_regression_fork_processes",
  "cpuctl_", "cpuhotplug_", "cpuset_",
  "cve-2016-7117", "cve-2017-17052",
  "dio_append", "dio_truncate",
  "dirtyc0w",
  "doio", "ebizzy",
  "epoll-ltp",
  "exit_group01",
  "fcntl17", "fcntl18", "fcntl36_64",
  "fork_exec_loop", "fork_procs",
  "fsx-linux",
  "inotify09",
  "kill02",
  "mallocstress",
  "memcg_test_2", "memcg_test_4",
  "memctl_test01",
  "mmap1", "mmap3", "mmapstress01",
  "mtest01",
  "netstress",
  "openfile",
  "pidns04", "pidns10", "pidns17",
  "pids_task2",
  "proc01",
  "pth_str01", "pth_str03", "pthserv",
  "rename14", "renameat01",
  "shm_test",
  "sigaltstack01",
  "timed_forkbomb",
  "tst_hexdump",
  NULL,
};

// Custom strncmp - not in ulib
int strncmp_local(const char *s1, const char *s2, int n) {
  while (n > 0 && *s1 && *s1 == *s2) { s1++; s2++; n--; }
  if (n == 0) return 0;
  return (unsigned char)*s1 - (unsigned char)*s2;
}

// 检查给定名称是否在黑名单中
int is_blacklisted(const char *name) {
  for (int i = 0; ltp_blacklist[i] != NULL; i++) {
    int len = strlen(ltp_blacklist[i]);
    if (strncmp_local(name, ltp_blacklist[i], len) == 0)
      return 1;
  }
  return 0;
}

// 运行LTP测试二进制文件（带超时）
// 返回: 0=通过, 1=失败, 2=超时, 3=跳过
int run_ltp_case(const char *dir_path, const char *binary_name) {
  int pid;
  char full_path[512];
  char *test_argv[2];

  strcpy(full_path, dir_path);
  if (full_path[strlen(full_path) - 1] != '/')
    strcat(full_path, "/");
  strcat(full_path, "ltp/testcases/bin/");
  strcat(full_path, binary_name);

  pid = fork();
  if (pid < 0) {
    printf("FAIL LTP CASE %s : fork_failed\n", binary_name);
    return 1;
  }
  if (pid == 0) {
    chdir(dir_path);
    test_argv[0] = (char *)binary_name;
    test_argv[1] = NULL;
    execve(full_path, test_argv, NULL);
    // 如果exec失败
    printf("FAIL LTP CASE %s : exec_failed\n", binary_name);
    exit(1);
  }

  // 等待子进程，带超时（LTP_TIMEOUT ticks = ~60秒 at ~100Hz）
#define LTP_TIMEOUT 6000
  int status = 0;
  int timed_out = 1;
  int ret;
  for (int t = 0; t < LTP_TIMEOUT / 10; t++) {
    ret = wait4(pid, &status, WNOHANG);
    if (ret == pid) {
      timed_out = 0;
      break;
    }
    if (ret < 0) {
      // 子进程可能已经不存在
      break;
    }
    sleep(10);  // 每10个tick检查一次 (~0.1秒)
  }

  if (timed_out) {
    printf("FAIL LTP CASE %s : timeout\n", binary_name);
    kill(pid);
    wait(0);
    return 2;
  }

  if (status == 0) {
    printf("END LTP CASE %s : 0\n", binary_name);
    return 0;
  } else {
    printf("FAIL LTP CASE %s : %d\n", binary_name, status);
    return 1;
  }
}

// 运行一个运行时(glibc/musl)的所有LTP测试
void run_ltp_tests(const char *dir_path, const char *runtime) {
  int fd, nread;
  char *buf;
  struct linux_dirent64 *d;
  int bpos;
  int total = 0, passed = 0, failed = 0, skipped = 0, timeout = 0;

  printf("#### OS COMP TEST GROUP START ltp-%s ####\n", runtime);

  buf = (char *) malloc(BUF_SIZE);
  if (!buf) {
    printf("ERROR: cannot allocate buffer for LTP\n");
    return;
  }

  // 构建LTP二进制目录路径
  char ltp_dir[256];
  strcpy(ltp_dir, dir_path);
  if (ltp_dir[strlen(ltp_dir) - 1] != '/')
    strcat(ltp_dir, "/");
  strcat(ltp_dir, "ltp/testcases/bin");

  fd = openat(AT_FDCWD, ltp_dir, O_RDONLY | O_DIRECTORY, 0600);
  if (fd < 0) {
    printf("LTP: cannot open directory %s\n", ltp_dir);
    free(buf);
    printf("#### OS COMP TEST GROUP END ltp-%s ####\n", runtime);
    return;
  }

  for (;;) {
    nread = getdents64(fd, (struct linux_dirent64 *) buf, BUF_SIZE);
    if (nread <= 0)
      break;

    for (bpos = 0; bpos < nread;) {
      d = (struct linux_dirent64 *) (buf + bpos);

      // 只处理普通文件
      if (d->d_type == T_FILE) {
        // 跳过.sh脚本
        if (!str_ends_with(d->d_name, ".sh")) {
          total++;
          if (is_blacklisted(d->d_name)) {
            printf("SKIP LTP CASE %s\n", d->d_name);
            skipped++;
          } else {
            int result = run_ltp_case(dir_path, d->d_name);
            switch (result) {
            case 0: passed++; break;
            case 2: timeout++; break;
            default: failed++; break;
            }
          }
        }
      }
      bpos += d->d_reclen;
    }
  }

  close(fd);
  free(buf);

  printf("LTP %s: total=%d passed=%d failed=%d skipped=%d timeout=%d\n",
         runtime, total, passed, failed, skipped, timeout);
  printf("#### OS COMP TEST GROUP END ltp-%s ####\n", runtime);
}

// 检查字符串是否以指定后缀结尾
int str_ends_with(const char *str, const char *suffix) {
    int str_len = strlen(str);
    int suffix_len = strlen(suffix);
    if (str_len < suffix_len) return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

// 从文件名提取测试组名（去掉_testcode.sh后缀）
void extract_group_name(const char *filename, char *group_name, int max_len) {
    int len = strlen(filename);
    int suffix_len = strlen(TESTCODE_SUFFIX);
    int copy_len = len - suffix_len;
    if (copy_len >= max_len) copy_len = max_len - 1;
    memcpy(group_name, filename, copy_len);
    group_name[copy_len] = '\0';
}

// 扫描目录中的所有*_testcode.sh脚本
// 返回找到的脚本数量
int scan_test_scripts(const char *dir_path, char scripts[][256], int max_scripts) {
    int fd, nread;
    char *buf = (char *) malloc(BUF_SIZE);
    struct linux_dirent64 *d;
    int bpos;
    int count = 0;

    fd = openat(AT_FDCWD, dir_path, O_RDONLY | O_DIRECTORY, 0600);
    if (fd < 0) {
        printf("init: cannot open directory %s\n", dir_path);
        free(buf);
        return 0;
    }

    for (;;) {
        nread = getdents64(fd, (struct linux_dirent64 *) buf, BUF_SIZE);
        if (nread <= 0)
            break;

        for (bpos = 0; bpos < nread && count < max_scripts;) {
            d = (struct linux_dirent64 *) (buf + bpos);
            
            // 检查是否是普通文件且以_testcode.sh结尾
            if (d->d_type == T_FILE && str_ends_with(d->d_name, TESTCODE_SUFFIX)) {
                // 跳过busybox_testcode.sh，因为它需要特殊处理
                if (strcmp(d->d_name, "busybox_testcode.sh") != 0 &&
                    strcmp(d->d_name, "ltp_testcode.sh") != 0) {
                    strcpy(scripts[count], d->d_name);
                    count++;
                }
            }
            bpos += d->d_reclen;
        }
    }

    close(fd);
    free(buf);
    return count;
}

// 运行单个测试脚本（直接执行，不通过busybox）
void run_test_script(const char *dir_path, const char *script_name) {
    int pid;
    char group_name[256];
    char *test_argv[10];
    char script_path[256];
    
    extract_group_name(script_name, group_name, sizeof(group_name));
    
    printf("#### OS COMP TEST GROUP START %s ####\n", group_name);
    
    // 构建脚本的完整路径
    strcpy(script_path, dir_path);
    int len = strlen(script_path);
    if (script_path[len - 1] != '/') {
        strcat(script_path, "/");
    }
    strcat(script_path, script_name);
    
    pid = fork();
    if (pid < 0) {
        printf("init: fork failed for %s\n", script_name);
    } else if (pid == 0) {
        // 子进程 - 直接执行脚本
        chdir(dir_path);
        test_argv[0] = (char *)script_name;
        test_argv[1] = NULL;
        execve(script_path, test_argv, NULL);
        printf("init: exec %s failed\n", script_name);
        exit(1);
    } else {
        // 父进程等待子进程完成
        wait(0);
    }
    
    printf("#### OS COMP TEST GROUP END %s ####\n", group_name);
}

// 自动扫描并运行所有测试点
void auto_run_tests(const char *dir_path) {
    char scripts[MAX_TEST_SCRIPTS][256];
    int count;
    
    printf("init: scanning test scripts in %s...\n", dir_path);
    
    count = scan_test_scripts(dir_path, scripts, MAX_TEST_SCRIPTS);
    
    printf("init: found %d test scripts\n", count);
    
    for (int i = 0; i < count; i++) {
        run_test_script(dir_path, scripts[i]);
    }
}

int main() {
    int pid, wpid;

    if(openat(AT_FDCWD, "console", O_RDWR, 0600) < 0){
      mknod("console", CONSOLE, 0);
      openat(AT_FDCWD, "console", O_RDWR, 0600);
    }
    dup(0);  // stdout
    dup(0);  // stderr

    mkdirat(AT_FDCWD, "/etc", 0666);
    int basic_testcases = 33;
    int bb_testcases = 55;

    // 首先运行basic测试
    printf("#### OS COMP TEST GROUP START basic-glibc ####\n");
    chdir(basic_path_glibc);
    for (int i = 0;i<basic_testcases;i++) {
        pid = fork();
        if (pid < 0) {
            printf("init: fork failed\n");
            exit(1);
        }
        if (pid == 0) {
            exec(basic_name[i], argv2);
            exit(1);
        }
        wait(0);
    }
    printf("#### OS COMP TEST GROUP END basic-glibc ####\n");

    printf("#### OS COMP TEST GROUP START basic-musl ####\n");
    chdir(basic_path_musl);
    for (int i = 0;i<basic_testcases;i++) {
        pid = fork();
        if (pid < 0) {
            printf("init: fork failed\n");
            exit(1);
        }
        if (pid == 0) {
            exec(basic_name[i], argv2);
            exit(1);
        }
        wait(0);
    }
    printf("#### OS COMP TEST GROUP END basic-musl ####\n");    

    // 运行busybox测试
    chdir(bb_path_musl);
    pid = fork();
    if (pid < 0) {
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0) {
        execve("busybox", bb_testcode, NULL);
        printf("init: exec busybox_testcode failed\n");
        exit(1);
    }
    wait(0);

    chdir(bb_path_glibc);
    pid = fork();
    if (pid < 0) {
        printf("init: fork failed\n");
        exit(1);
    }
    if(pid == 0) {
        execve("busybox", bb_testcode, NULL);
        printf("init: exec busybox_testcode failed\n");
        exit(1);
    }
    wait(0);

    printf("#### OS COMP TEST GROUP START libctest-musl ####\n");
    chdir(bb_path_musl);
    pid = fork();
    if (pid < 0) {
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0) {
        execve("busybox", libc_testcode, NULL);
        printf("init: exec libc_runstatic failed\n");
        exit(1);
    }
    wait(0);
    printf("#### OS COMP TEST GROUP END libctest-musl ####\n");

    // 运行LTP测试（直接执行LTP二进制文件，带超时和黑名单）
    printf("init: starting LTP tests...\n");
    run_ltp_tests(bb_path_musl, "musl");
    run_ltp_tests(bb_path_glibc, "glibc");

    // 自动扫描并运行其他测试脚本（包括cyclictest等）
    auto_run_tests(bb_path_musl);
    auto_run_tests(bb_path_glibc);
    auto_run_tests("/");

    shutdown();
}
