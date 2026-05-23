#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "fs/vfs/fs.h"
#include "fs/vfs/file.h"
#include "fs/inotify.h"
#include "defs.h"
#include "lib/string.h"

// Create a new inotify instance
int inotify_init1(int flags) {
    struct proc *p = myproc();
    struct inotify *in;
    struct file *f;
    int fd;

    // Allocate inotify structure
    in = (struct inotify *)kalloc();
    if (!in)
        return -12;  // -ENOMEM

    // Initialize inotify
    initlock(&in->lock, "inotify");
    in->next_wd = 1;
    in->event_len = 0;
    in->event_read_pos = 0;

    for (int i = 0; i < NOFILE; i++) {
        in->watches[i].wd = -1;
        in->watches[i].active = 0;
    }

    // Allocate a file structure
    f = filealloc();
    if (!f) {
        kfree((void *)in);
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
        kfree((void *)in);
        return -24;  // -EMFILE
    }

    f->f_type = FD_INOTIFY;
    f->inotify = in;

    return fd;
}

// Add a watch
int inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
    struct proc *p = myproc();
    struct file *f;
    struct inotify *in;
    int wd;

    if (fd < 0 || fd >= NOFILE)
        return -9;  // -EBADF

    f = p->ofile[fd];
    if (!f || f->f_type != FD_INOTIFY)
        return -9;  // -EBADF

    in = f->inotify;

    acquire(&in->lock);

    // Check if already watching this path
    for (int i = 0; i < NOFILE; i++) {
        if (in->watches[i].active && strncmp(in->watches[i].path, pathname, MAXPATH) == 0) {
            wd = in->watches[i].wd;
            in->watches[i].mask = mask;
            release(&in->lock);
            return wd;
        }
    }

    // Find free slot
    int slot = -1;
    for (int i = 0; i < NOFILE; i++) {
        if (!in->watches[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        release(&in->lock);
        return -28;  // -ENOSPC
    }

    // Allocate watch descriptor
    wd = in->next_wd++;

    // Initialize watch
    in->watches[slot].wd = wd;
    strncpy(in->watches[slot].path, pathname, MAXPATH);
    in->watches[slot].mask = mask;
    in->watches[slot].active = 1;

    release(&in->lock);
    return wd;
}

// Remove a watch
int inotify_rm_watch(int fd, int wd) {
    struct proc *p = myproc();
    struct file *f;
    struct inotify *in;

    if (fd < 0 || fd >= NOFILE)
        return -9;  // -EBADF

    f = p->ofile[fd];
    if (!f || f->f_type != FD_INOTIFY)
        return -9;  // -EBADF

    in = f->inotify;

    acquire(&in->lock);

    // Find and remove watch
    for (int i = 0; i < NOFILE; i++) {
        if (in->watches[i].active && in->watches[i].wd == wd) {
            in->watches[i].active = 0;
            in->watches[i].wd = -1;
            release(&in->lock);
            return 0;
        }
    }

    release(&in->lock);
    return -2;  // -ENOENT
}

// Read events from inotify
int inotify_read(struct file *f, uint64 addr, int n) {
    struct proc *p = myproc();
    struct inotify *in;
    int bytes_read = 0;

    if (!f->inotify)
        return -9;  // -EBADF

    in = f->inotify;

    acquire(&in->lock);

    // For now, return no events (simplified implementation)
    // In a full implementation, we would queue events and return them here

    release(&in->lock);

    return bytes_read;
}

// Close inotify instance
void inotify_close(struct file *f) {
    if (f->f_type == FD_INOTIFY && f->inotify) {
        kfree((void *)f->inotify);
        f->inotify = NULL;
    }
}
