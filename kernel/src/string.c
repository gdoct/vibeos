#include "kernel.h"

void *kmemset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char b = (unsigned char)c;
    while (n--) *d++ = b;
    return dst;
}

void *kmemcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int kmemcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

size_t kstrlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/*
 * GCC/Clang occasionally emit calls to the standard memXXX even with
 * -ffreestanding (e.g. for struct copies). Provide aliases so the
 * link succeeds without pulling in libc.
 */
extern "C" void *memset(void *d, int c, size_t n) __attribute__((alias("kmemset")));
extern "C" void *memcpy(void *d, const void *s, size_t n) __attribute__((alias("kmemcpy")));
extern "C" int   memcmp(const void *a, const void *b, size_t n) __attribute__((alias("kmemcmp")));
