#include "types.h"
#include "fs/stat.h"
#include "user.h"
#include "fs/fcntl.h"
#include "proc/signal.h"

struct sigaction act;

void aaa() {
    printf("111\n");
}

void bbb() {
    printf("222\n");
}

void handler(int signum);


int main() {
    printf("Start Test\n");
    int pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
    }
    if (pid == 0) {
        while (1) {
            printf("a");
        }
    }
    tkill(pid, SIGKILL);
    wait(0);
    printf("signal success\n");
    act.sa_handler = handler;
    printf("%p %p %p %p\n", handler, main, aaa, bbb);
    printf("%p\n", act.sa_handler);
    rt_sigaction(SIGILL, &act, NULL);

    tkill(getpid(), SIGILL);
    printf("Success\n");
}

void handler(int signum) {
    printf("Signal %d\n", signum);
    return ;
}
