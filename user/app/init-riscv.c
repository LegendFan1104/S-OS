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

char *git_env[10] = {"HOME=/user1", NULL};

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

const char *bb_test_success[] = {
  "echo \"#### independent command test\"",
  "ash -c exit",
  "sh -c exit",
  "basename /aaa/bbb",
  "cal",
  "clear",
  "date",
  "df",
  "dirname /aaa/bbb",
  "dmesg",
  "du",
  "expr 1 + 1",
  "false",
  "true",
  "which ls",
  "uname",
  "uptime",
  "printf \"abc\\n\"",
  "ps",
  "pwd",
  "free",
  "hwclock",
  "kill 10",
  "ls",
  "sleep 1",
  "echo \"#### file operation test\"",
  "touch test.txt",
  "echo \"hello world\" > test.txt",
  "cat test.txt",
  "cut -c 3 test.txt",
  "od test.txt",
  "head test.txt",
  "tail test.txt",
  "hexdump -C test.txt",
  "md5sum test.txt",
  "echo 'ccccccc' >> test.txt",
  "echo 'bbbbbbb' >> test.txt",
  "echo 'aaaaaaa' >> test.txt",
  "echo '2222222' >> test.txt",
  "echo '1111111' >> test.txt",
  "echo 'bbbbbbb' >> test.txt",
  "sort test.txt | busybox uniq",
  "stat test.txt",
  "strings test.txt",
  "wc test.txt",
  "[ -f test.txt ]",
  "more test.txt",
  "rm test.txt",
  "mkdir test_dir",
  "mv test_dir test",
  "rmdir test",
  "grep hello busybox_cmd.txt",
  "cp busybox_cmd.txt busybox_cmd.bak",
  "rm busybox_cmd.bak",
  "find . -name busybox_cmd.txt",
  NULL
};

char *bb_envp[] = {
  NULL,
};

char *libc_runstatic[10] = {"busybox", "sh", "run-static.sh", NULL};
char *libc_rundynamic[10] = {"busybox", "sh", "run-dynamic.sh", NULL};

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


    // // printf("#### OS COMP TEST GROUP END busybox-musl ####\n");

    shutdown();
}


//int main() {
//    int pid, wpid;
//
//    if(openat(AT_FDCWD, "dev/tty", O_RDWR, 0600) < 0){
//      mknod("dev/tty", CONSOLE, 0);
//      openat(AT_FDCWD, "dev/tty", O_RDWR, 0600);
//    }
//    dup(0);  // stdout
//    dup(0);  // stderr
//
//    mkdirat(AT_FDCWD, "/etc", 0666);
//
//    chdir("/mnt/glibc");
//    pid = fork();
//    if (pid < 0) {
//        printf("init: fork failed\n");
//        exit(1);
//    }
//    if(pid == 0) {
//        execve("busybox", final_test1, NULL);
//        printf("init: exec busybox_testcode failed\n");
//        exit(1);
//    }
//    wait(0);
//
//    chdir("/mnt/musl");
//    pid = fork();
//    if (pid < 0) {
//        printf("init: fork failed\n");
//        exit(1);
//    }
//    if(pid == 0) {
//        execve("busybox", final_test1, NULL);
//        printf("init: exec busybox_testcode failed\n");
//        exit(1);
//    }
//    wait(0);
//
//    chdir("/mnt/glibc");
//    pid = fork();
//    if (pid < 0) {
//        printf("init: fork failed\n");
//        exit(1);
//    }
//    if(pid == 0) {
//        execve("busybox", final_test2, NULL);
//        printf("init: exec final_test2 failed\n");
//        exit(1);
//    }
//    wait(0);
//
//    chdir("/mnt/musl");
//    pid = fork();
//    if (pid < 0) {
//        printf("init: fork failed\n");
//        exit(1);
//    }
//    if(pid == 0) {
//        execve("busybox", final_test2, NULL);
//        printf("init: exec final_test2 failed\n");
//        exit(1);
//    }
//    wait(0);
//
//    chdir("/mnt/glibc");
//    pid = fork();
//    if (pid < 0) {
//        printf("init: fork failed\n");
//        exit(1);
//    }
//    if(pid == 0) {
//        execve("busybox", final_test3, NULL);
//        printf("init: exec final_test2 failed\n");
//        exit(1);
//    }
//    wait(0);
//
//    chdir("/mnt/musl");
//    pid = fork();
//    if (pid < 0) {
//        printf("init: fork failed\n");
//        exit(1);
//    }
//    if(pid == 0) {
//        execve("busybox", final_test3, NULL);
//        printf("init: exec final_test2 failed\n");
//        exit(1);
//    }
//    wait(0);
//
//    shutdown();
//}

// int main() {
//     int pid, wpid;
//
//     if(openat(AT_FDCWD, "dev/tty", O_RDWR, 0600) < 0){
//       mknod("dev/tty", CONSOLE, 0);
//       openat(AT_FDCWD, "dev/tty", O_RDWR, 0600);
//     }
//     dup(0);  // stdout
//     dup(0);  // stderr
//
//     mkdirat(AT_FDCWD, "/etc", 0666);
//
//     chdir("/mnt/glibc");
//     // for (int i=0;i<2;i++) {
//     //     pid = fork();
//     //     if (pid < 0) {
//     //         printf("init: fork failed\n");
//     //         exit(1);
//     //     }
//     //     if(pid == 0) {
//     //         execve("./usr/bin/git", git_arg[i], NULL);
//     //         printf("init: exec busybox_testcode failed\n");
//     //         exit(1);
//     //     }
//     //     wait(0);
//     // }
//     //
//     // pid = fork();
//     // if (pid < 0) {
//     //     printf("init: fork failed\n");
//     //     exit(1);
//     // }
//     // if(pid == 0) {
//     //     execve("busybox", final_site, NULL);
//     //     printf("init: exec busybox_testcode failed\n");
//     //     exit(1);
//     // }
//     // wait(0);
//
//     for (int i=2;i<5;i++) {
//         pid = fork();
//         if (pid < 0) {
//             printf("init: fork failed\n");
//             exit(1);
//         }
//         if(pid == 0) {
//             execve("./usr/bin/git", git_arg[i], NULL);
//             printf("init: exec busybox_testcode failed\n");
//             exit(1);
//         }
//         wait(0);
//     }
//
//
//
//
//     shutdown();
// }

/*

*/
