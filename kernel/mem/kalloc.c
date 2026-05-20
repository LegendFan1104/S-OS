// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "mem/memlayout.h"
#include "lock/spinlock.h"
#include "platform.h"
#include "defs.h"
#include "mem/kalloc.h"

#include <mem/slab.h>

#include "lib/string.h"
#include "lib/print.h"
#include "proc/proc.h"
#include "mem/buddysystem.h"
#include "mem/slab.h"


uint64 pa_start;
struct spinlock memlock;

static uint64 pa2pgnm(void *pa) {
  if ((uint64)pa % PGSIZE != 0) {
    printf("pa %p\n", pa);
    panic("kfree!");
  }
  return ((uint64)pa - pa_start) / PGSIZE;
}

static void *pgnm2pa(int pgnm) {
  if (pgnm < 0) {
    return 0;
  }
  return (void *)((uint64)pgnm * PGSIZE + pa_start);
}

void kinit() {
  //多核情况下应该加锁
  initlock(&memlock, "memlock");
  buddysystem_init();
  // slab_init();
}

//从buddysystem中分配，需要额外的一步，从page num -> pa
void
kfree(void *pa)
{
  if ((uint64)pa == 1) {
    return ;
  }
  uint64 pgnm = pa2pgnm(pa);
  if (pgnm >= PGNUM) {
    struct proc *p = myproc();
    printf("kfree: out-of-range pa=%p pgnm=%p pa_start=%p caller=%p proc=%s pid=%d\n",
           pa, pgnm, pa_start, (uint64)__builtin_return_address(0),
           p ? p->name : "<none>", p ? p->pid : -1);
    panic("kfree: invalid page");
  }
  acquire(&memlock);
  // memset(pa, 0, PGSIZE);
  buddyfree(bs, (int)pgnm);
  // printf("free: %p\n", pa);
  release(&memlock);
}

//从buddysystem中分配1页
void *
kalloc(void)
{
  acquire(&memlock);
  int x = buddyalloc(bs, 0);
  if (x < 0) {
    release(&memlock);
    return 0;
  }
  void *pa = pgnm2pa(x);
  memset(pa, 0, PGSIZE);
  release(&memlock);
  // printf("alloc:%p\n",pa);
  return pa;
}

static int size_to_page_num(uint64 size) {
  return size / PGSIZE + (size % PGSIZE != 0);
}

/*
 *TODO: 动态内存分配
 */
void *kmalloc(uint64 size) {
  int num = size_to_page_num(size);
  acquire(&memlock);
  int x = buddyalloc(bs, num);
  if (x < 0) {
    release(&memlock);
    return 0;
  }
  void *pa = pgnm2pa(x);
  memset(pa, 0, num * PGSIZE);
  release(&memlock);
  // printf("alloc:%p\n",pa);
  return pa;
}
void *kcalloc(uint n, uint64 size) {
  int num = size_to_page_num(size);
  acquire(&memlock);
  int x = buddyalloc(bs, num);
  if (x < 0) {
    release(&memlock);
    return 0;
  }
  void *pa = pgnm2pa(x);
  memset(pa, 0, num * PGSIZE);
  release(&memlock);
  // printf("alloc:%p\n",pa);
  return pa;
}
