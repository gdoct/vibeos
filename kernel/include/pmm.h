#ifndef VIBEOS_PMM_H
#define VIBEOS_PMM_H

#include <stdint.h>
#include <stddef.h>
#include "../../boot/include/bootinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_SIZE   4096ULL
#define PAGE_MASK   (PAGE_SIZE - 1)
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_MASK) & ~PAGE_MASK)
#define PAGE_ALIGN_DOWN(x) ((x) & ~PAGE_MASK)

/*
 * Initialise the physical page allocator from the UEFI memory map.
 *
 * We use a bump allocator over the largest contiguous EfiConventionalMemory
 * region that starts above kernel_phys_end. That keeps day-one boot-up trivial
 * — a real freelist allocator is a follow-up.
 */
void pmm_init(const BootInfo *bi);

/* Allocate `pages` contiguous 4 KiB pages. Returns physical addr or 0.
   Single-page requests are served from the freelist when possible. */
uint64_t pmm_alloc_pages(size_t pages);

/* Allocate / return a single 4 KiB page. pmm_free_page only accepts
   page-aligned addresses (single pages); multi-page bump allocations
   can't be freed piecemeal. */
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys);

/* Diagnostics. */
uint64_t pmm_free_bytes(void);
uint64_t pmm_total_bytes(void);

/* Arena extent (the contiguous region the allocator hands pages from). Used by
   the page-refcount table (paging.c) to size and index its array. All
   allocatable pages — and therefore every user page — lie within this range. */
uint64_t pmm_arena_base(void);
uint64_t pmm_arena_pages(void);

#ifdef __cplusplus
}
#endif

#endif
