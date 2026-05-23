#pragma once

#include "types.h"
#include "lock/spinlock.h"

// eventfd flags
#define EFD_SEMAPHORE     1
#define EFD_CLOEXEC       02000000
#define EFD_NONBLOCK      04000

// eventfd structure
struct eventfd {
    struct spinlock lock;
    uint64_t count;
    int flags;
};

// Function prototypes
int eventfd2(unsigned int initval, int flags);
int eventfd_read(struct file *f, uint64 addr, int n);
int eventfd_write(struct file *f, uint64 addr, int n);
void eventfd_close(struct file *f);
