#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "fs/vfs/fs.h"
#include "fs/vfs/file.h"
#include "fs/vfs/inode.h"
#include "fs/memfd.h"
#include "defs.h"
#include "lib/string.h"

// Simple memory-backed file structure
struct memfd {
    char *data;
    int size;
    int capacity;
    char name[256];
};

// Create a memory-backed file
int memfd_create(const char *name, unsigned int flags) {
    struct proc *p = myproc();
    struct memfd *mfd;
    struct file *f;
    int fd;

    // Allocate memfd structure
    mfd = (struct memfd *)kalloc();
    if (!mfd)
        return -12;  // -ENOMEM

    // Initialize memfd
    mfd->capacity = 4096;  // Start with one page
    mfd->data = (char *)kalloc();
    if (!mfd->data) {
        kfree((void *)mfd);
        return -12;  // -ENOMEM
    }
    mfd->size = 0;
    if (name)
        strncpy(mfd->name, (char*)name, 256);
    else
        mfd->name[0] = '\0';

    // Allocate a file structure
    f = filealloc();
    if (!f) {
        kfree((void *)mfd->data);
        kfree((void *)mfd);
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
        kfree((void *)mfd->data);
        kfree((void *)mfd);
        return -24;  // -EMFILE
    }

    f->f_type = FD_MEMFD;
    f->memfd = mfd;

    return fd;
}

// Read from memfd
int memfd_read(struct file *f, uint64 addr, int n) {
    struct proc *p = myproc();
    struct memfd *mfd;
    int r;

    if (!f->memfd)
        return -9;  // -EBADF

    mfd = f->memfd;

    if (f->f_pos >= mfd->size)
        return 0;

    r = MIN(n, mfd->size - f->f_pos);
    if (copyout(p->pagetable, addr, mfd->data + f->f_pos, r) < 0)
        return -14;  // -EFAULT

    f->f_pos += r;
    return r;
}

// Write to memfd
int memfd_write(struct file *f, uint64 addr, int n) {
    struct proc *p = myproc();
    struct memfd *mfd;

    if (!f->memfd)
        return -9;  // -EBADF

    mfd = f->memfd;

    // Check if we need to expand
    if (f->f_pos + n > mfd->capacity) {
        // For simplicity, don't expand beyond initial capacity
        n = mfd->capacity - f->f_pos;
        if (n <= 0)
            return -28;  // -ENOSPC
    }

    if (copyin(p->pagetable, mfd->data + f->f_pos, addr, n) < 0)
        return -14;  // -EFAULT

    f->f_pos += n;
    if (f->f_pos > mfd->size)
        mfd->size = f->f_pos;

    return n;
}

// Close memfd
void memfd_close(struct file *f) {
    if (f->f_type == FD_MEMFD && f->memfd) {
        struct memfd *mfd = f->memfd;
        if (mfd->data)
            kfree((void *)mfd->data);
        kfree((void *)mfd);
        f->memfd = NULL;
    }
}
