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
#define PTE_COW  (1ULL << 9)    /* OS bit: copy-on-write (read-only, shared) */

/* Highest userspace virtual address + 1. The low (user) half of the canonical
   address space is [0, 0x0000_8000_0000_0000); copy_*_user rejects anything
   at/above this so a bogus user pointer can never reach kernel memory. */
#define USER_ADDR_END   0x0000800000000000ULL

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
vmspace_t *vmspace_fork(vmspace_t *parent);          /* copy-on-write share */
void       vmspace_destroy(vmspace_t *vm);           /* not while active */

/* ---- Copy-on-write + page refcounts (ROADMAP §1.1). ----
 * Every arena page carries a refcount = (number of PTEs pointing at it) - 1,
 * so a privately-mapped page has refcount 0. vmspace_fork shares pages read-only
 * with PTE_COW set and bumps the count; a write fault on such a page is repaired
 * by paging_cow_fault (private copy, or just re-grant write if sole owner). */
void page_refcount_init(void);

/* Handle a user write fault at `va` in `vm`. Returns 1 if it was a COW page and
   has now been made writable (the faulting instruction should be retried), 0 if
   the fault is not a COW case (genuine protection violation). */
int  paging_cow_fault(vmspace_t *vm, uint64_t va);

/* Drop one reference to physical page `pa` (an arena page that backed a user
   mapping), freeing it once the last reference goes away. */
void paging_unref_page(uint64_t pa);

/* ---- Validated user/kernel copies (ROADMAP §1.1). ----
 * Move `n` bytes between a kernel buffer and user virtual addresses in `vm`,
 * checking every page is present and user-accessible (and, for writes,
 * breaking COW first) before touching it. Data moves through the direct map, so
 * these are correct regardless of which CR3 is live and never fault in the
 * kernel. Return 0 on success, -1 if any part of the user range is unmapped /
 * not user / (for writes) read-only and not COW. */
long copy_from_user(void *kdst, vmspace_t *vm, uint64_t usrc, uint64_t n);
long copy_to_user(vmspace_t *vm, uint64_t udst, const void *ksrc, uint64_t n);

/* Validate that [addr, addr+n) is entirely user-accessible in `vm` (breaking COW
   for write==1 so the pages become privately writable). Returns 1 if the whole
   range is usable, 0 otherwise. Lets a syscall hand a user buffer straight to a
   driver/FS that reads or writes it through the (active) user VA without risking
   a kernel fault. */
int  paging_user_ok(vmspace_t *vm, uint64_t addr, uint64_t n, int write);

/* Copy a NUL-terminated string from user space into `kdst` (capacity `max`,
   always NUL-terminated on success). Returns the string length (excluding NUL),
   -1 on a bad user address, or -2 if it did not fit in `max`. */
long strncpy_from_user(char *kdst, vmspace_t *vm, uint64_t usrc, uint64_t max);

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
