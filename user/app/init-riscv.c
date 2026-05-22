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
                if (strcmp(d->d_name, "busybox_testcode.sh") != 0) {
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

    // 自动扫描并运行其他测试脚本（包括cyclictest等）
    auto_run_tests(bb_path_musl);
    auto_run_tests(bb_path_glibc);
    auto_run_tests("/");

    shutdown();
}
