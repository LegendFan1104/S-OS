#ifndef __PMEM_H__
#define __PMEM_H__
#include "types.h"
#include "list.h"

#define MAX_ORDER 11
#define BUDDY_MAX_ORDER 11

typedef struct buddy_node
{
    uint64 addr;
    int order;
    int refcnt;
    struct list_elem elem;
} buddy_node_t;

typedef struct buddy_system
{
    uint64 mem_start;
    uint64 mem_end;
    uint64 total_pages;
    uint64 *bitmap;
    buddy_node_t *nodes;
    struct list free_lists[BUDDY_MAX_ORDER + 1];
} buddy_system_t;

void pmem_init();
void *pmem_alloc_pages(int npages);
void pmem_free_pages(void *ptr, int npages);
void pmem_inc_ref(void *ptr);
void *kmalloc(uint64 size);
void *kcalloc(uint n, uint64 size);
void kfree(void *ptr);
void *kalloc(void);

int buddy_init(uint64 start, uint64 end);
void *buddy_alloc(int order);
void buddy_free(void *ptr, int order);
int get_order(uint64 size);
uint64 get_buddy_addr(uint64 addr, int order);
int is_buddy_free(uint64 addr, int order);
void set_buddy_used(uint64 addr, int order);
void set_buddy_free(uint64 addr, int order);
void buddy_check_integrity();
void buddy_diagnose_fork_issue();
void buddy_safe_check();
void buddy_cleanup_and_rebuild();
void buddy_free_tracked(void *ptr, int order, int caller_pid);

extern int debug_buddy;

#endif
