#include "types.h"
#include "platform.h"
#include "defs.h"
#include "param.h"
#include "mem/memlayout.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "mem/mem.h"
#include "mem/buddysystem.h"


uint64
sys_brk(void)
{
    struct proc *p = myproc();
    uint64 old_brk;
    uint64 new_brk;
    int64 delta;

    argaddr(0, &new_brk);

    old_brk = p->sz;
    if (new_brk == 0) {
        return old_brk;
    }

    // Keep user heap below user stack guard.
    if (new_brk >= USTACK_GURAD_PAGE) {
        return old_brk;
    }

    delta = (int64)new_brk - (int64)old_brk;

    // Our current buddy allocator manages PGNUM pages in total. A single
    // brk jump larger than that can never succeed and would spend a long time
    // in uvmalloc before failing; fail fast and let libc fallback paths run.
    if (delta > 0 && (uint64)delta > ((uint64)PGNUM * PGSIZE)) {
        return old_brk;
    }

    if (growproc(delta) < 0) {
        // Linux brk syscall returns current break on failure.
        return old_brk;
    }

    return p->sz;
}



uint64 sys_mmap(void) {
    uint64 start;
    int length, prot, flags, fd, offset;
    struct file* f;
    struct proc* p = myproc();
    uint64 err = 0xffffffffffffffff;

    argaddr(0, &start);
    argint(1, &length);
    argint(2, &prot);
    argint(3, &flags);
    argint(4, &fd);
    argint(5, &offset);

    if (fd == -1) {
        f = NULL;
    } else {
        f = p -> ofile[fd];
    }
    // printf("start: %p length:%d fd:%d path:%s flags:%d\n", start, length, fd, myproc()->ofile[fd]->f_path ,flags);
    if(start != 0 || offset != 0 || length < 0)
        return err;

    if((f != NULL && get_fops()->writable(f) == 0) && (prot & PROT_WRITE) != 0)
        return err;

    if (length == 0) {
        if (f == NULL) {
            length = 30 * PGSIZE;
        } else {
            return err;
        }
    }

    if(p->sz + length > MAXVA) {
        return err;
    }

    for(int i=0;i<NVMA;i++) {
        if(!p->vma[i].used) {
            p->vma[i].used = 1;

            p->vma[i].addr = PGROUNDUP(p->sz);
            p->vma[i].len = length;
            p->vma[i].flags = flags;
            p->vma[i].prot = prot;
            p->vma[i].vfile = f;
            p->vma[i].vfd = fd;
            p->vma[i].offset = offset;
            // printf("start: %p length:%d path:%s\n", p->vma[i].addr, length, p->ofile[fd]->f_path);
            p->sz = PGROUNDUP(p->sz) + length;
            if (f != NULL) {
                // printf("%d\n", i);
               get_fops()->dup(f);
            }

            return p->vma[i].addr;
        }
    }

    return err;
}

uint64 sys_munmap(void) {
    uint64 addr,length;

    argaddr(0, &addr);
    argaddr(1, &length);

    int i;
    struct proc *p = myproc();

    /*
     *目前不支持将mmap区域从中间断开
     */
    for(int i = 0;i < NVMA; i++) {
        if (p->vma[i].used == 0) continue;
        if(addr == p->vma[i].addr) {
            p->vma[i].addr += length;
            p->vma[i].len -= length;
            break;
        } else if(addr + length == p->vma[i].addr + p->vma[i].len) {
            p->vma[i].len -= length;
            break;
        }
    }

    if(i == NVMA) {
        return -1;
    }

    if(p->vma[i].flags == MAP_SHARED && (p->vma[i].prot & PROT_WRITE) != 0) {
        get_fops()->write(p->vma[i].vfile, addr, length);
    }
    // printf("%p %d\n", addr, length);
    if (addr % PGSIZE == 0) {
        uvmunmap(p->pagetable, addr, length / PGSIZE, 1);
    }

    if(p->vma[i].len == 0) {
        if (p->vma[i].vfile != NULL) {
            get_fops()->close(p->vma[i].vfile);
        }

        p->vma[i].used = 0;
    }

    return 0;

}

//unmap + mmap
uint64 sys_mremap(void) {
    uint64 oldaddr;
    int oldsize;
    int newsize;
    int flags;
    uint64 newaddr;
    argaddr(0, &oldaddr);
    argint(1, &oldsize);
    argint(2, &newsize);
    argint(3, &flags);

    struct proc *p = myproc();

    for (int i=0;i<NVMA;i++) {
        if (p->vma[i].addr == oldaddr) {
            if(p->vma[i].flags == MAP_SHARED && (p->vma[i].prot & PROT_WRITE) != 0) {
                get_fops()->write(p->vma[i].vfile, oldaddr, p->vma[i].len);
            }

            uvmunmap(p->pagetable, oldaddr, p->vma[i].len / PGSIZE, 1);


            p->vma[i].addr = p->sz;
            p->vma[i].len = newsize;

            p->sz += newsize;

            return p->vma[i].addr;
        }
    }


    return 0;
}

uint64 sys_madvise(void) {
    return 0;
}


uint64 sys_mprotect(void) {
    uint64 addr, len;
    int prot;
    argaddr(0, &addr);
    argaddr(1, &len);
    argint(2, &prot);


    int perm = 0;
    if (prot & PROT_READ) {
        perm |= PTE_R;
    }
    if (prot & PROT_WRITE) {
        perm |= PTE_W;
    }
    if (prot & PROT_EXEC) {
        perm |= PTE_X;
    }
    if (protectpages(myproc()->pagetable, addr, len, perm) < 0) {
        return -1;
    }

    return 0;
}












