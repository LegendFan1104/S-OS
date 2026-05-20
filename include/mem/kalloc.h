#pragma once
#include "types.h"


extern uint64 pa_start;



// kalloc.c
void *kalloc(void);
void kfree(void *);
void kinit(void);
void *kmalloc(uint64 size);
void *kcalloc(uint n, uint64 size);