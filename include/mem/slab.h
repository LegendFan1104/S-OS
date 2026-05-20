#pragma once
#include "types.h"
#include "lock/spinlock.h"
#include "param.h"
#include "lib/list.h"

//系统中的slab cache暂时只支持 16 32 64 128 256等5种字节大小
//slab 的metadata 放在分配的page的开头部分
struct slab {
    void *pa_start;
    uint64 free_objs_count; //有多少个空余元素
    uint64 max_objs_count;

    list_head_t list;

    uint64 first_obj;
    char is_empty;
};

struct slab_cache {
    uint32 size; // 这个slab cache所分配对象的大小

    uint32 free_slabs_count;
    list_head_t free_slabs, partial_slabs, full_slabs;
};

#define DEFAULT_MAX_FREE_SLABS_ALLOWED 5
#define LIST2SLAB(x) (struct slab *)PGROUNDDOWN((uint64)(&(x)))
#define is_slab_full(slab)			\
((slab)->free_objs_count == 0)

#define is_slab_empty(slab)		\
((slab)->free_objs_count == (slab)->max_objs_count)

extern struct slab_cache *slab_cache[5];


void slab_cache_free(struct slab_cache *cache, void *pa);
void *slab_cache_alloc(struct slab_cache *cache);
void slab_init();
int get_slab_cache_index(uint64 size);