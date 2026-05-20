#include "mem/slab.h"

#include "platform.h"

#include "mem/kalloc.h"

struct slab_cache *slab_cache[5];


static struct slab* get_owning_slab(void *pa) {
    return (struct slab *)PGROUNDDOWN((uint64)pa);
}

//对一个slab新分配的page进行
void page_free_obj_init(void *page, uint32 obj_size, uint64 page_end) {
    uint64 current_addr = (uint64)page;
    uint64 next_addr = (uint64)page + obj_size;
    for (;next_addr + obj_size < page_end; next_addr += obj_size,current_addr += obj_size) {
        *(uint64 *)current_addr = next_addr;
    }

    *(uint64 *)current_addr = 0;
}

//一个slab管理一个page
static struct slab *create_slab(uint32 obj_size) {
    struct slab *slab = kalloc();

    //为metadata预留空间
    int cnt = 1;
    while (cnt * obj_size <= sizeof(struct slab)) cnt++;

    list_init(&(slab->list));
    slab->pa_start = (void *)((uint64)slab + cnt * sizeof(struct slab)); //真正分配的内存
    uint64 page_end = (uint64)slab + PGSIZE;
    slab->free_objs_count = (PGSIZE - cnt * sizeof(struct slab)) / obj_size;
    slab->first_obj = (uint64)slab->pa_start;
    slab->max_objs_count = slab->free_objs_count;
    page_free_obj_init(slab->pa_start, obj_size, page_end);

    return slab;
}

static void destroy_slab(struct slab *slab) {
    kfree(slab);
}

static void *slab_alloc(struct slab *slab) {
    uint64 obj = slab->first_obj;
    slab->free_objs_count--;

    slab->first_obj = *(uint64 *)obj;

    return (void*)obj;
}

static void slab_free(struct slab *slab, void *obj) {
    slab->free_objs_count++;
    *(uint64 *)obj = slab->first_obj;
    slab->first_obj = (uint64)obj;
}

static void default_slab_memory_recycle(struct slab_cache *cache) {
    struct slab *slab;
    for (;cache->free_slabs_count > DEFAULT_MAX_FREE_SLABS_ALLOWED; cache->free_slabs_count--) {
        /*
         *这里能这样获得slab地址的方法基于slab的元数据放在page的起始地址
         *通过listhead的地址可以计算出来slab的首地址
         *如果一个slab管理超过一个页可能需要对这里的代码进行修改
         */
        slab = LIST2SLAB(cache->free_slabs.next);
        list_del(&(slab->list));
        destroy_slab(slab);
    }
}

struct slab_cache * slab_cache_new(uint32 size) {
    struct slab_cache *cache = kalloc();

    cache -> size = size;
    cache -> free_slabs_count = 0;

    list_init(&(cache->free_slabs));
    list_init(&(cache->partial_slabs));
    list_init(&(cache->full_slabs));
}

void *slab_cache_alloc(struct slab_cache *cache) {
    if (!list_empty(&(cache->partial_slabs))) {
        struct slab *slab = LIST2SLAB(cache->partial_slabs.next);

        void *pa = slab_alloc(slab);

        if (is_slab_full(slab)) {
            list_del(&(slab->list));
            list_add(&(slab->list), &(cache->full_slabs));
        }
        return pa;
    } else {
        //需要构造一个新的slab
        if (list_empty(&(cache->free_slabs))) {
            struct slab *slab = create_slab(cache->size);
            list_add(&(slab->list), &(cache->free_slabs));
            cache->free_slabs_count++;
        }
        //从free_slabs中分配内存
        struct slab *slab = LIST2SLAB(cache->free_slabs.next);
        void *pa = slab_alloc(slab);

        list_del(&(slab->list));
        list_add(&(slab->list), &(cache->partial_slabs));
    }
    return NULL;
}

void slab_cache_free(struct slab_cache *cache, void *pa) {
    struct slab *slab = get_owning_slab(pa);
    char slab_was_full = is_slab_full(slab);
    slab_free(slab, pa);
    char slab_now_empty = is_slab_empty(slab);

    if (!slab_was_full && slab_now_empty) {
        //partial -> free
        list_del(&(slab->list));
        list_add(&(slab->list), &(cache->free_slabs));
    } else if (slab_was_full) {
        //full -> partial
        list_del(&(slab->list));
        list_add(&(slab->list), &(cache->partial_slabs));
    }
    default_slab_memory_recycle(cache);
}

void slab_init() {
    slab_cache[0] = slab_cache_new(16);
    slab_cache[1] = slab_cache_new(32);
    slab_cache[2] = slab_cache_new(64);
    slab_cache[3] = slab_cache_new(128);
    slab_cache[4] = slab_cache_new(256);
}

int get_slab_cache_index(uint64 size) {
    if (size <= 16) {
        return 0;
    } else if (size <= 32) {
        return 1;
    } else if (size <= 64) {
        return 2;
    } else if (size <= 128) {
        return 3;
    } else if (size <= 256) {
        return 4;
    }
    return -1;
}

















