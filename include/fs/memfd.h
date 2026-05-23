#pragma once

#include "types.h"

// memfd flags
#define MFD_CLOEXEC       0x0001
#define MFD_ALLOW_SEALING 0x0002
#define MFD_HUGETLB       0x0004

// Function prototypes
int memfd_create(const char *name, unsigned int flags);
int memfd_read(struct file *f, uint64 addr, int n);
int memfd_write(struct file *f, uint64 addr, int n);
void memfd_close(struct file *f);
