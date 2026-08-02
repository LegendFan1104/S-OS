#ifndef DEFS_H
#define DEFS_H

#include "types.h"

struct file;
struct pipe;

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)

#ifndef FINAL_DEV_DIAG
#define FINAL_DEV_DIAG 0
#endif

#ifndef STACK_COPYOUT_DIAG
#define STACK_COPYOUT_DIAG 0
#endif

// console.c
void            chardev_init(void);
void            consoleintr(int);
void            consputc(int);

// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

#endif // DEF_H
