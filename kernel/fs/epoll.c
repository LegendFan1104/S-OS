#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "fs/vfs/fs.h"
#include "fs/vfs/file.h"
#include "fs/epoll.h"
#include "defs.h"

// Create a new epoll instance
int epoll_create1(int flags) {
    struct proc *p = myproc();
    struct epoll *ep;
    struct file *f;
    int fd;

    // Allocate epoll structure
    ep = (struct epoll *)kalloc();
    if (!ep)
        return -12;  // -ENOMEM

    // Initialize epoll
    initlock(&ep->lock, "epoll");
    ep->head = NULL;
    ep->count = 0;

    for (int i = 0; i < NOFILE; i++) {
        ep->items[i].fd = -1;
        ep->items[i].active = 0;
        ep->items[i].next = NULL;
        ep->items[i].prev = NULL;
    }

    // Allocate a file structure
    f = filealloc();
    if (!f) {
        kfree((void *)ep);
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
        kfree((void *)ep);
        return -24;  // -EMFILE
    }

    f->f_type = FD_EPOLL;
    f->epoll = ep;

    return fd;
}

// Control interface for epoll
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    struct proc *p = myproc();
    struct file *epfile;
    struct epoll *ep;
    struct epoll_item *item;

    if (epfd < 0 || epfd >= NOFILE)
        return -9;  // -EBADF

    epfile = p->ofile[epfd];
    if (!epfile || epfile->f_type != FD_EPOLL)
        return -9;  // -EBADF

    ep = epfile->epoll;

    if (fd < 0 || fd >= NOFILE)
        return -9;  // -EBADF

    if (!p->ofile[fd])
        return -9;  // -EBADF

    if (epfd == fd)
        return -22;  // -EINVAL

    acquire(&ep->lock);

    switch (op) {
    case EPOLL_CTL_ADD:
        // Check if fd already registered
        for (int i = 0; i < NOFILE; i++) {
            if (ep->items[i].active && ep->items[i].fd == fd) {
                release(&ep->lock);
                return -17;  // -EEXIST
            }
        }

        // Find free slot
        item = NULL;
        for (int i = 0; i < NOFILE; i++) {
            if (!ep->items[i].active) {
                item = &ep->items[i];
                break;
            }
        }

        if (!item) {
            release(&ep->lock);
            return -12;  // -ENOMEM
        }

        item->fd = fd;
        item->event = *event;
        item->active = 1;

        // Add to list
        item->next = ep->head;
        item->prev = NULL;
        if (ep->head)
            ep->head->prev = item;
        ep->head = item;
        ep->count++;
        break;

    case EPOLL_CTL_DEL:
        // Find and remove fd
        item = NULL;
        for (int i = 0; i < NOFILE; i++) {
            if (ep->items[i].active && ep->items[i].fd == fd) {
                item = &ep->items[i];
                break;
            }
        }

        if (!item) {
            release(&ep->lock);
            return -2;  // -ENOENT
        }

        // Remove from list
        if (item->prev)
            item->prev->next = item->next;
        else
            ep->head = item->next;

        if (item->next)
            item->next->prev = item->prev;

        item->active = 0;
        item->fd = -1;
        ep->count--;
        break;

    case EPOLL_CTL_MOD:
        // Find fd and modify
        item = NULL;
        for (int i = 0; i < NOFILE; i++) {
            if (ep->items[i].active && ep->items[i].fd == fd) {
                item = &ep->items[i];
                break;
            }
        }

        if (!item) {
            release(&ep->lock);
            return -2;  // -ENOENT
        }

        item->event = *event;
        break;

    default:
        release(&ep->lock);
        return -22;  // -EINVAL
    }

    release(&ep->lock);
    return 0;
}

// Wait for events on epoll instance
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask) {
    struct proc *p = myproc();
    struct file *epfile;
    struct epoll *ep;
    struct epoll_item *item;
    int nevents = 0;
    uint64 start_tick = ticks;
    uint64 timeout_ticks;

    if (epfd < 0 || epfd >= NOFILE)
        return -9;  // -EBADF

    if (maxevents <= 0 || maxevents > NOFILE)
        return -22;  // -EINVAL

    epfile = p->ofile[epfd];
    if (!epfile || epfile->f_type != FD_EPOLL)
        return -9;  // -EBADF

    ep = epfile->epoll;

    // Convert timeout to ticks (HZ is typically 100)
    int hz = 100;  // Default HZ value
    if (timeout < 0)
        timeout_ticks = 0xFFFFFFFFFFFFFFFFULL;  // Infinite
    else
        timeout_ticks = (timeout * hz) / 1000;

    // Save current signal mask if provided
    sigset_t old_mask;
    if (sigmask) {
        old_mask = p->block;
        p->block = *sigmask;
    }

    while (nevents == 0) {
        acquire(&ep->lock);

        // Check all registered fds for events
        for (item = ep->head; item && nevents < maxevents; item = item->next) {
            if (!item->active)
                continue;

            int fd = item->fd;
            if (fd < 0 || fd >= NOFILE)
                continue;

            struct file *f = p->ofile[fd];
            if (!f)
                continue;

            uint32_t revents = 0;

            // For simplicity, just report readable/writable
            // In a full implementation, we'd check the actual file state
            revents = item->event.events & (EPOLLIN | EPOLLOUT);

            if (revents) {
                events[nevents].events = revents;
                events[nevents].data = item->event.data;
                nevents++;

                // Handle EPOLLONESHOT
                if (item->event.events & EPOLLONESHOT) {
                    item->event.events &= ~EPOLLIN;
                    item->event.events &= ~EPOLLOUT;
                }
            }
        }

        release(&ep->lock);

        if (nevents > 0)
            break;

        // Check timeout
        if (timeout >= 0 && (ticks - start_tick) >= timeout_ticks)
            break;

        // Check for signals
        if (p->signal) {
            if (sigmask)
                p->block = old_mask;
            return -4;  // -EINTR
        }

        // Yield CPU
        yield();
    }

    // Restore signal mask
    if (sigmask)
        p->block = old_mask;

    return nevents;
}

// Close epoll instance
void epoll_close(struct epoll *ep) {
    if (!ep)
        return;

    acquire(&ep->lock);

    // Clean up all items
    struct epoll_item *item = ep->head;
    while (item) {
        struct epoll_item *next = item->next;
        item->active = 0;
        item->fd = -1;
        item = next;
    }

    release(&ep->lock);
    kfree((void *)ep);
}

// Notify epoll about events on a fd (called from file operations)
int epoll_notify(int fd, uint32_t events) {
    // This is a simplified version - in a full implementation,
    // we would need to track which epoll instances are watching this fd
    // For now, epoll_pwait will poll the fds
    return 0;
}
