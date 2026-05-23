#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "fs/vfs/fs.h"
#include "fs/vfs/file.h"
#include "fs/eventfd.h"
#include "sys/fcntl.h"
#include "defs.h"

#define EFD_MAX_COUNT 0xFFFFFFFFFFFFFFFFULL

// Create a new eventfd
int eventfd2(unsigned int initval, int flags) {
    struct proc *p = myproc();
    struct eventfd *efd;
    struct file *f;
    int fd;

    // Allocate eventfd structure
    efd = (struct eventfd *)kalloc();
    if (!efd)
        return -12;  // -ENOMEM

    // Initialize eventfd
    initlock(&efd->lock, "eventfd");
    efd->count = initval;
    efd->flags = flags;

    // Allocate a file structure
    f = filealloc();
    if (!f) {
        kfree((void *)efd);
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
        kfree((void *)efd);
        return -24;  // -EMFILE
    }

    f->f_type = FD_EVENTFD;
    f->eventfd = efd;

    return fd;
}

// File operations for eventfd
int eventfd_read(struct file *f, uint64 addr, int n) {
    struct proc *p = myproc();
    struct eventfd *efd;
    uint64_t val;

    if (!f->eventfd)
        return -9;  // -EBADF

    if (n < sizeof(uint64_t))
        return -22;  // -EINVAL

    efd = f->eventfd;

    acquire(&efd->lock);

    // Block until count is non-zero (unless non-blocking)
    while (efd->count == 0) {
        release(&efd->lock);
        if (f->f_flags & O_NONBLOCK)
            return -11;  // -EAGAIN
        yield();
        acquire(&efd->lock);
    }

    // Read the counter value
    val = efd->count;

    // If EFD_SEMAPHORE, decrement by 1, otherwise reset to 0
    if (efd->flags & EFD_SEMAPHORE)
        efd->count = 1;
    else
        efd->count = 0;

    release(&efd->lock);

    if (copyout(p->pagetable, addr, (char*)&val, sizeof(val)) < 0)
        return -14;  // -EFAULT

    return sizeof(uint64_t);
}

int eventfd_write(struct file *f, uint64 addr, int n) {
    struct proc *p = myproc();
    struct eventfd *efd;
    uint64_t val;

    if (!f->eventfd)
        return -9;  // -EBADF

    if (n < sizeof(uint64_t))
        return -22;  // -EINVAL

    if (copyin(p->pagetable, (char*)&val, addr, sizeof(val)) < 0)
        return -14;  // -EFAULT

    // Check for overflow
    if (val == EFD_MAX_COUNT)
        return -22;  // -EINVAL

    efd = f->eventfd;

    acquire(&efd->lock);

    // Check if adding would overflow
    if (EFD_MAX_COUNT - efd->count < val) {
        release(&efd->lock);
        return -75;  // -EOVERFLOW
    }

    efd->count += val;

    release(&efd->lock);

    return sizeof(uint64_t);
}

void eventfd_close(struct file *f) {
    if (f->f_type == FD_EVENTFD && f->eventfd) {
        kfree((void *)f->eventfd);
        f->eventfd = NULL;
    }
}
