#include "types.h"
#include "param.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "fs/vfs/inode.h"
#include "fs/ext4/vfs_ext4_ext.h"

struct {
    struct spinlock lock;
    struct inode inode[NINODE];
} itable;

void inodeinit()
{
    int i = 0;
    // printf("--- %d ---\n", sizeof(struct inode));
    initlock(&itable.lock, "itable");
    for(i = 0; i < NINODE; i++) {
        initlock(&itable.inode[i].lock, "inode");
        itable.inode[i].i_op = get_ext4_inode_op();
    }
}

struct inode *get_inode() {
    int i;
    acquire(&itable.lock);
    for(i = 0; i < NINODE; i++) {
        if (itable.inode[i].i_valid == 0) {
            itable.inode[i].i_valid = 1;
            break;
        }
    }
    release(&itable.lock);
    if (i == NINODE) {
        return NULL;
    }
    return &itable.inode[i];
}
struct inode *free_inode(struct inode *inode) {
    acquire(&itable.lock);
    inode->i_valid = 0;
    release(&itable.lock);
}
































