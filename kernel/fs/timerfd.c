#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "fs/vfs/fs.h"
#include "fs/vfs/file.h"
#include "fs/timerfd.h"
#include "sys/fcntl.h"
#include "defs.h"

#define NSEC_PER_SEC 1000000000L

// Helper function to convert timespec to ticks
static uint64_t timespec_to_ticks(const struct timespec *ts) {
    if (!ts || ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= NSEC_PER_SEC)
        return 0;
    int hz = 100;  // Default HZ
    return (ts->tv_sec * hz) + (ts->tv_nsec * hz / NSEC_PER_SEC);
}

// Helper function to convert ticks to timespec
static void ticks_to_timespec(uint64_t ticks_val, struct timespec *ts) {
    if (!ts)
        return;
    int hz = 100;  // Default HZ
    ts->tv_sec = ticks_val / hz;
    ts->tv_nsec = (ticks_val % hz) * NSEC_PER_SEC / hz;
}

// Create a new timerfd
int timerfd_create(int clockid, int flags) {
    struct proc *p = myproc();
    struct timerfd *tfd;
    struct file *f;
    int fd;

    // Validate clockid
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC)
        return -22;  // -EINVAL

    // Allocate timerfd structure
    tfd = (struct timerfd *)kalloc();
    if (!tfd)
        return -12;  // -ENOMEM

    // Initialize timerfd
    initlock(&tfd->lock, "timerfd");
    tfd->clockid = clockid;
    tfd->flags = flags;
    tfd->expirations = 0;
    tfd->armed = 0;
    tfd->interval = 0;
    tfd->next_expiry = 0;

    // Allocate a file structure
    f = filealloc();
    if (!f) {
        kfree((void *)tfd);
        return -12;  // -ENOMEM
    }

    // Allocate a file descriptor
    for(fd = 0; fd < NOFILE; fd++){
        if(p->ofile[fd] == 0){
            p->ofile[fd] = f;
            break;
        }
    }
    if(fd == NOFILE){
        fileclose(f);
        kfree((void *)tfd);
        return -24;  // -EMFILE
    }

    f->f_type = FD_TIMERFD;
    f->timerfd = tfd;

    return fd;
}

// Set timer
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value, struct itimerspec *old_value) {
    struct proc *p = myproc();
    struct file *f;
    struct timerfd *tfd;

    if (fd < 0 || fd >= NOFILE)
        return -9;  // -EBADF

    f = p->ofile[fd];
    if (!f || f->f_type != FD_TIMERFD)
        return -9;  // -EBADF

    tfd = f->timerfd;

    acquire(&tfd->lock);

    // Return old value if requested
    if (old_value) {
        ticks_to_timespec(tfd->interval, &old_value->it_interval);
        if (tfd->armed)
            ticks_to_timespec(tfd->next_expiry > ticks ? tfd->next_expiry - ticks : 0, &old_value->it_value);
        else
            old_value->it_value.tv_sec = old_value->it_value.tv_nsec = 0;
    }

    // Disarm timer if new_value is zero
    if (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_nsec == 0) {
        tfd->armed = 0;
        tfd->interval = 0;
        tfd->expirations = 0;
        release(&tfd->lock);
        return 0;
    }

    // Set new timer
    uint64_t ticks_val = timespec_to_ticks(&new_value->it_value);
    if (ticks_val == 0 && (new_value->it_value.tv_sec != 0 || new_value->it_value.tv_nsec != 0)) {
        release(&tfd->lock);
        return -22;  // -EINVAL
    }

    tfd->interval = timespec_to_ticks(&new_value->it_interval);
    tfd->next_expiry = ticks + ticks_val;
    tfd->armed = 1;
    tfd->expirations = 0;

    release(&tfd->lock);
    return 0;
}

// Get timer
int timerfd_gettime(int fd, struct itimerspec *curr_value) {
    struct proc *p = myproc();
    struct file *f;
    struct timerfd *tfd;

    if (fd < 0 || fd >= NOFILE)
        return -9;  // -EBADF

    f = p->ofile[fd];
    if (!f || f->f_type != FD_TIMERFD)
        return -9;  // -EBADF

    tfd = f->timerfd;

    acquire(&tfd->lock);

    ticks_to_timespec(tfd->interval, &curr_value->it_interval);

    if (tfd->armed)
        ticks_to_timespec(tfd->next_expiry > ticks ? tfd->next_expiry - ticks : 0, &curr_value->it_value);
    else
        curr_value->it_value.tv_sec = curr_value->it_value.tv_nsec = 0;

    release(&tfd->lock);
    return 0;
}

// Read from timerfd
int timerfd_read(struct file *f, uint64 addr, int n) {
    struct proc *p = myproc();
    struct timerfd *tfd;
    uint64_t expirations;

    if (!f->timerfd)
        return -9;  // -EBADF

    if (n < sizeof(uint64_t))
        return -22;  // -EINVAL

    tfd = f->timerfd;

    acquire(&tfd->lock);

    // Block until timer expires (unless non-blocking)
    while (tfd->expirations == 0) {
        release(&tfd->lock);
        if (f->f_flags & O_NONBLOCK)
            return -11;  // -EAGAIN
        yield();
        acquire(&tfd->lock);
    }

    // Return number of expirations
    expirations = tfd->expirations;
    tfd->expirations = 0;

    release(&tfd->lock);

    if (copyout(p->pagetable, addr, (char*)&expirations, sizeof(expirations)) < 0)
        return -14;  // -EFAULT

    return sizeof(uint64_t);
}

// Close timerfd
void timerfd_close(struct file *f) {
    if (f->f_type == FD_TIMERFD && f->timerfd) {
        kfree((void *)f->timerfd);
        f->timerfd = NULL;
    }
}

// Check and update timerfd timers (called from clock interrupt)
void timerfd_check_expiry(void) {
    // This would be called periodically to check for expired timers
    // For simplicity, we check on read instead
}
