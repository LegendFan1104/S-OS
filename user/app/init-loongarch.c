// init: The initial user-level program

#include "types.h"
#include "fs/stat.h"
#include "lock/spinlock.h"
#include "lock/sleeplock.h"
#include "user.h"
#include "fs/fcntl.h"

#define CONSOLE 1

char *argv[] = { "sh", 0 };
char *argv2[] = {"", 0};
char *argv3[] = {"/mnt/", 0};

char basic_path_musl[] = "/mnt/musl/";
char basic_path_glibc[] = "/glibc/basic/";
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
    chdir("/loongarch/musl/");
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

    chdir("/loongarch/glibc/");
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

    // printf("#### OS COMP TEST GROUP START libctest-musl ####\n");
    // chdir(bb_path_musl);
    // pid = fork();
    // if (pid < 0) {
    //     printf("init: fork failed\n");
    //     exit(1);
    // }
    // if(pid == 0) {
    //     execve("busybox", libc_testcode, NULL);
    //     printf("init: exec libc_runstatic failed\n");
    //     exit(1);
    // }
    // wait(0);
    // printf("#### OS COMP TEST GROUP END libctest-musl ####\n");



    shutdown();
}


// int main() {
//     int pid, wpid;
//
//     if(openat(AT_FDCWD, "dev/tty", O_RDWR, 0600) < 0){
//         mknod("dev/tty", CONSOLE, 0);
//         openat(AT_FDCWD, "dev/tty", O_RDWR, 0600);
//     }
//     dup(0);  // stdout
//     dup(0);  // stderr
//
//     mkdirat(AT_FDCWD, "/etc", 0666);
//
//     chdir("/mnt/musl");
//     pid = fork();
//     if (pid < 0) {
//         printf("init: fork failed\n");
//         exit(1);
//     }
//     if(pid == 0) {
//         execve("busybox", final_test1, NULL);
//         printf("init: exec busybox_testcode failed\n");
//         exit(1);
//     }
//     wait(0);
//
//     chdir("/mnt/glibc");
//     pid = fork();
//     if (pid < 0) {
//         printf("init: fork failed\n");
//         exit(1);
//     }
//     if(pid == 0) {
//         execve("busybox", final_test1, NULL);
//         printf("init: exec busybox_testcode failed\n");
//         exit(1);
//     }
//     wait(0);
//
//     chdir("/mnt/glibc");
//     pid = fork();
//     if (pid < 0) {
//         printf("init: fork failed\n");
//         exit(1);
//     }
//     if(pid == 0) {
//         execve("busybox", final_test2, NULL);
//         printf("init: exec final_test2 failed\n");
//         exit(1);
//     }
//     wait(0);
//
//     chdir("/mnt/musl");
//     pid = fork();
//     if (pid < 0) {
//         printf("init: fork failed\n");
//         exit(1);
//     }
//     if(pid == 0) {
//         execve("busybox", final_test2, NULL);
//         printf("init: exec final_test2 failed\n");
//         exit(1);
//     }
//     wait(0);
//
//     chdir("/mnt/glibc");
//     pid = fork();
//     if (pid < 0) {
//         printf("init: fork failed\n");
//         exit(1);
//     }
//     if(pid == 0) {
//         execve("busybox", final_test3, NULL);
//         printf("init: exec final_test2 failed\n");
//         exit(1);
//     }
//     wait(0);
//
//     chdir("/mnt/musl");
//     pid = fork();
//     if (pid < 0) {
//         printf("init: fork failed\n");
//         exit(1);
//     }
//     if(pid == 0) {
//         execve("busybox", final_test3, NULL);
//         printf("init: exec final_test2 failed\n");
//         exit(1);
//     }
//     wait(0);
//
//
//     shutdown();
// }
//

