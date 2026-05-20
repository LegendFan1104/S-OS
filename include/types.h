#pragma once

typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;

typedef long int64;

typedef int64 intptr_t;

typedef uint64 uintptr_t;

typedef unsigned long size_t;

typedef long ssize_t;

typedef unsigned char *byte_pointer;

typedef int bool;

typedef uint64 uint64_t;

typedef uint32 uint32_t;

typedef uint16 uint16_t;

typedef uint8 uint8_t;

typedef int int32_t;

typedef short int16_t;

#define false 0
#define true 1

typedef uint64 pde_t;

#define NULL ((void *)0)

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};