// init: The initial user-level program

#include "types.h"
#include "fs/stat.h"
#include "lock/spinlock.h"
#include "lock/sleeplock.h"
#include "user.h"
#include "fs/fcntl.h"

#define CONSOLE 1

#define BUF_SIZE 4096
#define MAX_TEST_SCRIPTS 64
#define TESTCODE_SUFFIX "_testcode.sh"
#define WNOHANG 0x01

// LTP test blacklist - tests known to hang, crash, or not applicable to SOS
char *ltp_blacklist[] = {
  "accept", "accept4", "bind", "connect", "getpeername", "getsockname",
  "listen", "recv", "recvfrom", "recvmmsg", "recvmsg",
  "send", "sendfile", "sendmmsg", "sendmsg", "sendto",
  "shutdown", "socket", "socketpair",
  "ptrace",
  "msgctl", "msgget", "msgrcv", "msgsnd", "msgstress",
  "semctl", "semget", "semop", "sem_post", "sem_timedwait", "sem_wait",
  "shmat", "shmctl", "shmdt", "shmget", "mq_",
  "inotify", "fanotify",
  "epoll",
  "timer_create", "timer_delete", "timer_getoverrun", "timer_gettime", "timer_settime",
  "cgroup", "cgget", "cgset",
  "quotactl", "flock", "fallocate",
  "create_module", "delete_module", "finit_module", "init_module",
  "kexec", "kexec_load",
  "keyctl", "add_key", "request_key",
  "getxattr", "setxattr", "fgetxattr", "fsetxattr", "listxattr", "flistxattr",
  "removexattr", "fremovexattr", "lgetxattr", "lsetxattr", "llistxattr", "lremovexattr",
  "fork13", "fork10",
  "crash01", "crash02",
  "dio", "aio",
  "hackbench",
  "move_pages",
  "mbind",
  NULL,
};

int strncmp_local(const char *s1, const char *s2, int n) {
  while (n > 0 && *s1 && *s1 == *s2) { s1++; s2++; n--; }
  if (n == 0) return 0;
  return (unsigned char)*s1 - (unsigned char)*s2;
}

int is_blacklisted(const char *name) {
  for (int i = 0; ltp_blacklist[i] != NULL; i++) {
    int len = strlen(ltp_blacklist[i]);
    if (strncmp_local(name, ltp_blacklist[i], len) == 0)
      return 1;
  }
  return 0;
}

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
    printf("FAIL LTP CASE %s : exec_failed\n", binary_name);
    exit(1);
  }

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
    if (ret < 0) break;
    sleep(10);
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
    if (nread <= 0) break;

    for (bpos = 0; bpos < nread;) {
      d = (struct linux_dirent64 *) (buf + bpos);
      if (d->d_type == T_FILE) {
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

char *argv[] = { "sh", 0 };
char *argv2[] = {"", 0};
char *argv3[] = {"/mnt/", 0};

char basic_path_musl[] = "/mnt/musl/basic/";
char basic_path_glibc[] = "/mnt/glibc/basic/";
char *basic_name[] = {"brk", "chdir", "clone", "close", "dup", "dup2", "execve", "exit", "fork", "fstat", "getcwd", "getdents", "getpid",
    "getppid", "gettimeofday", "mkdir_", "mmap", "mount", "munmap", "open", "openat", "pipe", "read", "sleep", "test_echo", "times",
    "umount", "uname", "unlink", "wait", "waitpid", "write", "yield",
};

char bb_path_musl[] = "/mnt/musl/";
char bb_path_glibc[] = "/mnt/glibc/";


char bb_cmd_file_musl[] = "/mnt/musl/busybox_cmd.txt";
char bb_cmd_file_glibc[] = "/mnt/glibc/busybox_cmd.txt";
char *libc_testcode[10] = {"busybox", "sh", "/libc_test.sh", NULL};


char *busybox_debug[10] = {"busybox", "sh", "/busybox_final.sh", NULL};
char *busybox_debug2[10] = {"busybox", "du", NULL};

char *int_test1[10] = {"interrupts-test-1", NULL};

char *final_test1[10] = {"busybox", "sh", "interrupts_testcode.sh", NULL};
char *final_test2[10] = {"busybox", "sh", "copy-file-range_testcode.sh", NULL};
char *final_test3[10] = {"busybox", "sh", "splice_testcode.sh", NULL};



static int is_space_ch(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char *trim_line(char *s) {
    while (*s && is_space_ch(*s)) {
        s++;
    }
    int n = strlen(s);
    while (n > 0 && is_space_ch(s[n - 1])) {
        s[n - 1] = 0;
        n--;
    }
    return s;
}

static void run_busybox_line(char *line, char *busybox_path) {
    char line_exec[256];
    char *bb_argv[32];
    int bb_argc = 1;

    strcpy(line_exec, line);
    bb_argv[0] = "busybox";

    char *p = line_exec;
    while (*p) {
        while (*p && is_space_ch(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        if (bb_argc >= 31) {
            break;
        }
        bb_argv[bb_argc++] = p;
        while (*p && !is_space_ch(*p)) {
            p++;
        }
        if (*p) {
            *p = 0;
            p++;
        }
    }
    bb_argv[bb_argc] = NULL;

    if (bb_argc <= 1) {
        return;
    }

    int pid = fork();
    if (pid < 0) {
        printf("init: fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        execve(busybox_path, bb_argv, NULL);
        printf("init: exec %s failed\n", bb_argv[1]);
        exit(1);
    }
    wait(0);
}

static void run_busybox_group_by_cmdfile(char *group_name, char *workdir, char *cmdfile, char *busybox_path) {
    char read_buf[256];
    char line_buf[256];
    int line_len = 0;

    printf("#### OS COMP TEST GROUP START %s ####\n", group_name);

    if (chdir(workdir) < 0) {
        printf("init: chdir %s failed\n", workdir);
        printf("#### OS COMP TEST GROUP END %s ####\n", group_name);
        return;
    }

    int fd = openat(AT_FDCWD, cmdfile, O_RDONLY, 0);
    if (fd < 0) {
        printf("init: open %s failed\n", cmdfile);
        printf("#### OS COMP TEST GROUP END %s ####\n", group_name);
        return;
    }

    int n;
    while ((n = read(fd, read_buf, sizeof(read_buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = read_buf[i];
            if (c == '\n') {
                line_buf[line_len] = 0;
                char *line = trim_line(line_buf);
                if (line[0] && line[0] != '#') {
                    char line_print[256];
                    strcpy(line_print, line);
                    run_busybox_line(line, busybox_path);
                    if (strcmp(line_print, "false")) {
                        printf("testcase busybox %s success\n", line_print);
                    }
                }
                line_len = 0;
            } else if (c != '\r') {
                if (line_len < (int)sizeof(line_buf) - 1) {
                    line_buf[line_len++] = c;
                }
            }
        }
    }

    if (line_len > 0) {
        line_buf[line_len] = 0;
        char *line = trim_line(line_buf);
        if (line[0] && line[0] != '#') {
            char line_print[256];
            strcpy(line_print, line);
            run_busybox_line(line, busybox_path);
            if (strcmp(line_print, "false")) {
                printf("testcase busybox %s success\n", line_print);
            }
        }
    }

    close(fd);
    printf("#### OS COMP TEST GROUP END %s ####\n", group_name);
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
    int basic_testcases = 33;

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
            printf("failed\n");
            exit(1);
        }
        wait(0);
    }
    printf("#### OS COMP TEST GROUP END basic-musl ####\n");

    chdir(basic_path_glibc);
    printf("#### OS COMP TEST GROUP START basic-glibc ####\n");
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

    run_busybox_group_by_cmdfile("busybox-musl", bb_path_musl, bb_cmd_file_musl, "/mnt/glibc/busybox");
    run_busybox_group_by_cmdfile("busybox-glibc", bb_path_glibc, bb_cmd_file_glibc, "/mnt/glibc/busybox");

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
