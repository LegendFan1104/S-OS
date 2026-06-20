#ifndef __ELF_H__
#define __ELF_H__

#include "types.h"

#define ELF_MAGIC 0x464C457FU

typedef struct elf_header {
    uint32 magic;
    uint8 elf[12];
    uint16 type;
    uint16 machine;
    uint32 version;
    uint64 entry;
    uint64 phoff;
    uint64 shoff;
    uint32 flags;
    uint16 ehsize;
    uint16 phentsize;
    uint16 phnum;
    uint16 shentsize;
    uint16 shnum;
    uint16 shstrndx;
} elf_header_t;

typedef struct program_header {
    uint32 type;
    uint32 flags;
    uint64 off;
    uint64 vaddr;
    uint64 paddr;
    uint64 filesz;
    uint64 memsz;
    uint64 align;
} program_header_t;

// Program header types.
#define ELF_PROG_LOAD           1
#define ELF_PROG_INTERP         3
#define ELF_PROG_PHDR           6

// ELF file types.
#define ELF_TYPE_EXEC           2
#define ELF_TYPE_DYN            3

// exec() argument limits.
#define NENV                    64
#define NARG                    16

// Auxiliary vector entries.
#define AT_NULL                 0
#define AT_IGNORE               1
#define AT_EXECFD               2
#define AT_PHDR                 3
#define AT_PHENT                4
#define AT_PHNUM                5
#define AT_PAGESZ               6
#define AT_BASE                 7
#define AT_FLAGS                8
#define AT_ENTRY                9
#define AT_NOTELF               10
#define AT_UID                  11
#define AT_EUID                 12
#define AT_GID                  13
#define AT_EGID                 14
#define AT_PLATFORM             15
#define AT_HWCAP                16
#define AT_CLKTCK               17
#define AT_FPUCW                18
#define AT_DCACHEBSIZE          19
#define AT_ICACHEBSIZE          20
#define AT_UCACHEBSIZE          21
#define AT_IGNOREPPC            22
#define AT_SECURE               23
#define AT_BASE_PLATFORM        24
#define AT_RANDOM               25
#define AT_HWCAP2               26
#define AT_EXECFN               31
#define AT_SYSINFO              32
#define AT_SYSINFO_EHDR         33

#endif
