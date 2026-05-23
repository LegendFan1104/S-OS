#pragma once

#include "types.h"
#include "param.h"
#include "lock/spinlock.h"

// epoll constants
#define EPOLL_CLOEXEC       O_CLOEXEC

#define EPOLLIN             0x001
#define EPOLLPRI            0x002
#define EPOLLOUT            0x004
#define EPOLLERR            0x008
#define EPOLLHUP            0x010
#define EPOLLRDNORM         0x040
#define EPOLLRDBAND         0x080
#define EPOLLWRNORM         0x100
#define EPOLLWRBAND         0x200
#define EPOLLMSG            0x400
#define EPOLLRDHUP          0x2000

#define EPOLLEXCLUSIVE      (1U << 28)
#define EPOLLWAKEUP         (1U << 29)
#define EPOLLONESHOT        (1U << 30)
#define EPOLLET             (1U << 31)

// epoll_ctl opcodes
#define EPOLL_CTL_ADD       1
#define EPOLL_CTL_DEL       2
#define EPOLL_CTL_MOD       3

// epoll_event structure
struct epoll_event {
    uint32_t events;
    uint64_t data;
};

// epoll item - represents a registered fd
struct epoll_item {
    int fd;
    struct epoll_event event;
    struct epoll_item *next;
    struct epoll_item *prev;
    int active;
};

// epoll instance structure
struct epoll {
    struct spinlock lock;
    struct epoll_item items[NOFILE];  // Max files per process
    struct epoll_item *head;          // Active items list
    int count;
};

// Function prototypes
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask);
void epoll_close(struct epoll *ep);
int epoll_notify(int fd, uint32_t events);
