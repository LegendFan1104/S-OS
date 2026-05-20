#include "mem/buddysystem.h"
#include "mem/memlayout.h"
#include "mem/kalloc.h"
#include "lib/string.h"
#include "lib/print.h"
#include "defs.h"
#include "proc/proc.h"

extern char end[]; // first address after kernel.
// defined by kernel.ld.

/*
 *通过一个类似线段树的类型对物理页号进行维护
 *log的复杂度完成一次页的分配和回收
 */
struct buddysystem *bs;


static int is_pow_of_2(uint32 x) {
    return !(x & (x-1));
}

static int next_pow_of_2(uint32 x) {
    if ( is_pow_of_2(x) )
        return x;
    x |= x>>1;
    x |= x>>2;
    x |= x>>4;
    x |= x>>8;
    x |= x>>16;
    return x+1;
}

static int _index_offset(int index, int level, int max_level) {
    return ((index + 1) - (1 << level)) << (max_level - level);
}


void buddysystem_init() {
    // pagen = next_pow_of_2((PHYSTOP - (uint64)end) / PGSIZE);
    pa_start = PGROUNDUP((uint64)end);
    bs = (struct buddysystem *)pa_start;
    pa_start += BSSIZE * PGSIZE;
    memset(bs, 0, BSSIZE * PGSIZE);
    while (!((1 << bs->level) & PGNUM)) bs->level++; //线段树有多少层
    // printf("pa_start: %p\n", pa_start);
}

static int index_offset(int index, int level, int max_level) {
    return ((index + 1) - (1 << level)) << (max_level - level);
}

static void mark_parent(struct buddysystem *self, int index) {
    while (1) {
        int buddy = index - 1 + (index & 1) * 2;
        if (buddy > 0 && (self->tree[buddy] == NODE_USED || self->tree[buddy] == NODE_FULL)) {
            index = (index + 1) / 2 - 1; //父节点
            self->tree[index] = NODE_FULL;
        } else {
            return ;
        }
    }
}

//从buddysystem中分配s个page
int buddyalloc(struct buddysystem *self, int s) {
    int size;
    if (s==0) {
        size = 1;
    } else {
        size = (int)next_pow_of_2(s);
    }
    int length = 1 << self->level;

    if (size > length)
        return -1;

    int index = 0;
    int level = 0;

    while (index >= 0) {
        if (size == length) {
            if (self->tree[index] == NODE_UNUSED) {
                self->tree[index] = NODE_USED;
                mark_parent(self, index);
                return _index_offset(index, level, self->level);
            }
        } else {
            // size < length
            switch (self->tree[index]) {
                case NODE_USED:
                case NODE_FULL:
                    break;
                case NODE_UNUSED:
                    // split first
                        self->tree[index] = NODE_SPLIT;
                self->tree[index*2+1] = NODE_UNUSED;
                self->tree[index*2+2] = NODE_UNUSED;
                default:
                    index = index * 2 + 1;
                length /= 2;
                level++;
                continue;
            }
        }
        if (index & 1) {
            ++index;
            continue;
        }
        for (;;) {
            level--;
            length *= 2;
            index = (index+1)/2 -1;
            if (index < 0)
                return -1;
            if (index & 1) {
                ++index;
                break;
            }
        }
    }

    return -1;
}

static void combine(struct buddysystem *self, int index) {
    while (1) {
        int buddy = index - 1 + (index & 1) * 2;
        if (buddy < 0 || self->tree[buddy] != NODE_UNUSED) {
            self->tree[index] = NODE_UNUSED;
            while (((index = (index + 1) / 2 - 1) >= 0) &&  self->tree[index] == NODE_FULL){
                self->tree[index] = NODE_SPLIT;
            }
            return ;
        }
        index = (index + 1) / 2 - 1;
    }
}

//回收一个page
void buddyfree(struct buddysystem *self, int offset) {
    int req = offset;
    int total = 1 << self->level;
    if (offset < 0 || offset >= total) {
        struct proc *p = myproc();
        printf("buddyfree: invalid offset=%d total=%d caller=%p proc=%s pid=%d\n",
               offset, total, (uint64)__builtin_return_address(0),
               p ? p->name : "<none>", p ? p->pid : -1);
        panic("Free wrong page");
    }
    int left = 0;
    int length = total;
    int index = 0;
    while (1) {
        switch (self->tree[index]) {
            case NODE_USED:
                combine(self, index);
                return ;
            case NODE_UNUSED:
                {
                    struct proc *p = myproc();
                    printf("buddyfree: double free? req=%d left=%d len=%d idx=%d caller=%p proc=%s pid=%d\n",
                           req, left, length, index, (uint64)__builtin_return_address(0),
                           p ? p->name : "<none>", p ? p->pid : -1);
                }
                panic("Free wrong page");
                return ;
            default:
                length /= 2;
                if (offset < left + length) {
                    index = index * 2 + 1;
                } else {
                    left += length;
                    index = index * 2 + 2;
                }
                break;
        }
    }
}




















