#include "ctype.h"
#include "lib/string.h"

void *memset(void *dst, int c, uint n) {
    char *cdst = (char *) dst;
    int i;
    for (i = 0; i < n; i++) {
        cdst[i] = c;
    }
    return dst;
}

int memcmp(const void *v1, const void *v2, uint n) {
    const uchar *s1, *s2;

    s1 = v1;
    s2 = v2;
    while (n-- > 0) {
        if (*s1 != *s2)
            return *s1 - *s2;
        s1++, s2++;
    }

    return 0;
}

void *memmove(void *dst, const void *src, uint n) {
    const char *s;
    char *d;

    if (n == 0)
        return dst;

    s = src;
    d = dst;
    if (s < d && s + n > d) {
        s += n;
        d += n;
        while (n-- > 0) {
            *--d = *--s;
        }

    } else
        while (n-- > 0) {

            *d++ = *s++;
        }

    return dst;
}

// memcpy exists to placate GCC.  Use memmove.
int memcpy(void *dst, const void *src, uint n) {
    memmove(dst, src, n);
    return 0;
}

int strncmp(const char *p, const char *q, uint n) {
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (uchar) *p - (uchar) *q;
}

int strcmp(const char *p, const char *q) {
    while (*p && *p == *q)
        p++, q++;
    return (uchar) *p - (uchar) *q;
}

char *strcpy(char *s, const char *t) {
    char *os = s;
    while ((*s++ = *t++) != 0)
        ;
    return os;
}

// Like strncpy but guaranteed to NUL-terminate.
char *strncpy(char *s, const char *t, int n) {
    char *os = s;
    if (n <= 0)
        return os;
    while (n-- > 0 && (*s++ = *t++) != 0)
        ;
    *s = 0;
    return os;
}

int strlen(const char *s) {
    int n;

    for (n = 0; s[n]; n++)
        ;
    return n;
}

size_t strnlen(const char *s, size_t count) {
    const char *sc;

    for (sc = s; *sc != '\0' && count--; ++sc)
        /* nothing */;
    return sc - s;
}


void str_toupper(char *str) {
    if (str != NULL) {
        while (*str != '\0') {
            *str = toupper(*str);
            str++;
        }
    }
}

void str_tolower(char *str) {
    if (str != NULL) {
        while (*str != '\0') {
            *str = tolower(*str);
            str++;
        }
    }
}
char *strchr(const char *str, int c) {
    while (*str != '\0') {
        if (*str == (char) c) {
            return (char *) str;
        }
        str++;
    }
    if (c == '\0') {
        return (char *) str;
    }
    return NULL;
}

int str_split(const char *str, const char ch, char *str1, char *str2) {
    char *p = strchr(str, ch);
    if (p == NULL) {
        return -1;
    }
    strncpy(str1, str, p - str);
    strncpy(str2, p + 1, strlen(str) - 1 - (p - str));

    return 1;
}

char *strcat(char *dest, const char *src) {
    char *p = dest;
    while (*p) {
        ++p;
    }
    while (*src) {
        *p++ = *src++;
    }
    *p = '\0';
    return dest;
}

// Like strncpy but guaranteed to NUL-terminate.
char*
safestrcpy(char *s, const char *t, int n)
{
    char *os;

    os = s;
    if(n <= 0)
        return os;
    while(--n > 0 && (*s++ = *t++) != 0)
        ;
    *s = 0;
    return os;
}

//Conver n to string in s
//return the end pos
char* digit_tostring(char *s, int n) {
    int num = 0;
    int x = n;
    while (x) {
        num++;
        s++;
        x /= 10;
    }

    char *rs = s;
    s--;

    while (n) {
        int d = n % 10;
        *s = d + '0';
        s--;
        n /= 10;
    }
    return rs;
}


