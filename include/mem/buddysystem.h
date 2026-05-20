#pragma once

#include "lib/list.h"
#include "platform.h"


#define PAGE_ORDER 10
#define PGNUM (1 << 15) //物理内存的总页数，这里是128MB
#define BSSIZE 10 //buddysystem结构占用的内存页数

struct buddysystem {
    int level;
    uint8 tree[1];
};

extern struct buddysystem *bs;


void buddyfree(struct buddysystem *self, int offset);
int buddyalloc(struct buddysystem *self, int s);
void buddysystem_init();

#define NODE_UNUSED 0
#define NODE_USED 1
#define NODE_SPLIT 2
#define NODE_FULL 3
























