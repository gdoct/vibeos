#ifndef VIBEOS_KMALLOC_H
#define VIBEOS_KMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sub-page heap allocator. Powers-of-two size classes carved out of PMM
 * pages for small objects; whole-page allocations for anything larger.
 * Every block carries a 16-byte header so kfree can find its class.
 */
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Diagnostics: bytes currently handed out (payload, excluding headers). */
size_t kmalloc_in_use(void);

#ifdef __cplusplus
}
#endif

#endif
