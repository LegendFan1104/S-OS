#pragma once
#include "types.h"

int memcmp(const void *, const void *, uint);

void *memmove(void *, const void *, uint);

void *memset(void *, int, uint);

int memcpy(void *, const void *, uint);

char *strcpy(char *, const char *);

int strlen(const char *);

size_t strnlen(const char *s, size_t count);

int strncmp(const char *, const char *, uint);

int strcmp(const char *, const char *);

char *strncpy(char *, const char *, int);

void str_toupper(char *);

void str_tolower(char *);

char *strchr(const char *, int);

int str_split(const char *, char, char *, char *);

char *strcat(char *dest, const char *src);

char* safestrcpy(char *s, const char *t, int n);

char* digit_tostring(char *s, int n);