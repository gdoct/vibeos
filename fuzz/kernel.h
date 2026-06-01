#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static inline void *kmemcpy(void *d, const void *s, unsigned long n){ return memcpy(d,s,n); }
static inline void *kmemset(void *d, int c, unsigned long n){ return memset(d,c,n); }
static inline int kprintf(const char *,...){ return 0; }
