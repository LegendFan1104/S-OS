#pragma once

#include "types.h"
#include "lock/spinlock.h"

// timerfd flags
#define TFD_CLOEXEC       O_CLOEXEC
#define TFD_NONBLOCK      O_NONBLOCK

// timerfd timer types
#define TFD_TIMER_ABSTIME (1 << 0)
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)

// Clock IDs
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define CLOCK_REALTIME_ALARM     8
#define CLOCK_BOOTTIME_ALARM     9

// itimerspec structure
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

// timerfd structure
struct timerfd {
    struct spinlock lock;
    int clockid;
    int flags;
    uint64_t expirations;
    int armed;
    uint64_t interval;
    uint64_t next_expiry;
};

// Function prototypes
int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value, struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);
void timerfd_close(struct file *f);
int timerfd_read(struct file *f, uint64 addr, int n);
void timerfd_tick(void);
