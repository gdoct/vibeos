#ifndef VIBEOS_PAGING_H
#define VIBEOS_PAGING_H

#include <stdint.h>
#include <stddef.h>
#include "../../boot/include/bootinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Virtual memory layout (4-level paging, our own page tables):
 *
 *   0x0000_0000_0000_0000 .. identity map of low physical memory (2 MiB
 *                            pages). The kernel keeps running here — it is
 *                            still linked at its 1 MiB physical address.
 *   PHYS_OFFSET ..           direct map of the same physical memory, so
 *                            any phys addr is reachable as phys_to_virt(p).
 *   KSTACK_REGION ..         vmalloc-style window of 4 KiB pages used for
 *                            per-task kernel stacks, each preceded by an
 *                            unmapped guard page.
 */
#define PHYS_OFFSET     0xFFFF800000000000ULL
#define KSTACK_REGION   0xFFFFA00000000000ULL
#define KERNEL_VBASE    0xFFFFFFFF80000000ULL   /* -2 GiB; kernel image VA base */

/* Page-table entry flags. */
#define PTE_P    (1ULL << 0)    /* present */
#define PTE_W    (1ULL << 1)    /* writable */
#define PTE_U    (1ULL << 2)    /* user-accessible */
#define PTE_PWT  (1ULL << 3)    /* write-through */
#define PTE_PCD  (1ULL << 4)    /* cache disable */
#define PTE_PS   (1ULL << 7)    /* page size (2 MiB at PD level) */

/* Build our page tables (identity + direct map) and load CR3. Must run
   after pmm_init — it allocates table pages from the PMM. */
void paging_init(const BootInfo *bi);

/* Map / unmap `pages` 4 KiB pages at virtual `va` -> physical `pa`.
   Intermediate tables are allocated on demand from the PMM. */
void vmap(uint64_t va, uint64_t pa, size_t pages, uint64_t flags);
void vunmap(uint64_t va, size_t pages);

/* Resolve a virtual address through the active tables. Returns 1 and sets
   *pa_out if mapped (4 KiB or 2 MiB), 0 if not present. */
int  paging_query(uint64_t va, uint64_t *pa_out);

/* Translate a (mapped) kernel virtual address to its physical address.
   For places that need a physical address — e.g. virtio DMA descriptors —
   now that kernel pointers are direct-map / high-half VAs, never identity.
   Panics if `va` is unmapped. */
uint64_t kva_to_phys(const volatile void *va);

/* ---- Per-process address spaces (ROADMAP §3 Milestone B). ----
 * A vmspace owns a PML4 whose upper (kernel) half is shared with the master
 * tables; the lower half holds the process's private user mappings. */
typedef struct vmspace {
    uint64_t pml4_phys;
} vmspace_t;

uint64_t   paging_kernel_pml4(void);                 /* master PML4 (phys) */
vmspace_t *vmspace_create(void);                     /* kernel half shared */
void       vmspace_switch(vmspace_t *vm);            /* load CR3 (NULL = master) */
void       vmspace_map(vmspace_t *vm, uint64_t va, uint64_t pa,
                       size_t pages, uint64_t flags);
int        vmspace_query(vmspace_t *vm, uint64_t va, uint64_t *pa_out);
void       vmspace_unmap(vmspace_t *vm, uint64_t va, size_t pages);  /* PTEs only; caller frees pa */
vmspace_t *vmspace_fork(vmspace_t *parent);          /* eager deep copy */
void       vmspace_destroy(vmspace_t *vm);           /* not while active */

/* Allocate a kernel stack of `pages` 4 KiB pages in the KSTACK_REGION
   window, with an unmapped guard page just below it. Returns the stack
   TOP virtual address; *base_out gets the lowest stack address. */
uint64_t kstack_alloc(size_t pages, uint64_t *base_out);

static inline void *phys_to_virt(uint64_t pa) {
    return (void *)(uintptr_t)(PHYS_OFFSET + pa);
}
static inline uint64_t virt_to_phys(void *va) {
    return (uint64_t)(uintptr_t)va - PHYS_OFFSET;
}

#ifdef __cplusplus
}
#endif

#endif
