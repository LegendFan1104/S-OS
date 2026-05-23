#pragma once

#include "types.h"
#include "param.h"
#include "lock/spinlock.h"

// inotify flags
#define IN_CLOEXEC       O_CLOEXEC
#define IN_NONBLOCK      O_NONBLOCK

// inotify events
#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800

#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000

#define IN_ONLYDIR       0x01000000
#define IN_DONT_FOLLOW   0x02000000
#define IN_EXCL_UNLINK   0x04000000
#define IN_MASK_CREATE   0x10000000
#define IN_MASK_ADD      0x20000000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000

#define IN_ALL_EVENTS    (IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | \
                          IN_CLOSE_NOWRITE | IN_OPEN | IN_MOVED_FROM | IN_MOVED_TO | \
                          IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF)

// inotify_event structure
struct inotify_event {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
};

// inotify watch
struct inotify_watch {
    int wd;
    char path[MAXPATH];
    uint32_t mask;
    int active;
};

// inotify instance
struct inotify {
    struct spinlock lock;
    struct inotify_watch watches[NOFILE];
    int next_wd;
    // Event buffer (simplified - in real impl would be a queue)
    char event_buf[4096];
    int event_len;
    int event_read_pos;
};

// Function prototypes
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
int inotify_read(struct file *f, uint64 addr, int n);
void inotify_close(struct file *f);
void inotify_notify(const char *path, uint32_t mask);
