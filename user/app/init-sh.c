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

char basic_path_musl[] = "/mnt/musl/basic/";
char basic_path_glibc[] = "/glibc/basic/";
char bb_path_musl[] = "/mnt/musl/busybox";
char bb_path_glibc[] = "/glibc/busybox";
char *basic_name[] = {"brk", "chdir", "clone", "close", "dup", "dup2", "execve", "exit", "fork", "fstat", "getcwd", "getdents", "getpid",
    "getppid", "gettimeofday", "mkdir_", "mmap", "mount", "munmap", "open", "openat", "pipe", "read", "sleep", "test_echo", "times",
    "umount", "uname", "unlink", "wait", "waitpid", "write", "yield",
};

 int
 main(void)
 {
   int pid, wpid;

   if(openat(AT_FDCWD, "console", O_RDWR, 0600) < 0){
     mknod("console", CONSOLE, 0);
     openat(AT_FDCWD, "console", O_RDWR, 0600);
   }
   dup(0);  // stdout
   dup(0);  // stderr

  for(;;){
     printf("init: starting sh\n");
     pid = fork();
     if(pid < 0){
       printf("init: fork failed\n");
       exit(1);
     }
     if(pid == 0){
       execve("sh", argv, 0);
       printf("init: exec sh failed\n");
       exit(1);
     }

    for(;;){
       // this call to wait() returns if the shell exits,
       // or if a parentless process exits.
       wpid = wait((int *) 0);
       if(wpid == pid){
         // the shell exited; restart it.
         break;
       } else if(wpid < 0){
         printf("init: wait returned an error\n");
         exit(1);
       } else {
         // it was a parentless process; do nothing.
       }
     }
   }
 }

