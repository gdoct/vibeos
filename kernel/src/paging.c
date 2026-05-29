#include "kernel.h"
#include "pmm.h"
#include "irq.h"
#include "paging.h"

/*
 * 4-level (PML4) paging — higher-half kernel (ROADMAP §3, Phase 0).
 *
 * The kernel runs at KERNEL_VBASE (-2 GiB) and reaches all physical memory
 * through the PHYS_OFFSET direct map, so the entire low half is free for
 * userspace. The master tables map:
 *   - PML4[256] -> direct map  (VA == PHYS_OFFSET + PA), covering >=4 GiB
 *                  plus the framebuffer/MMIO.
 *   - PML4[511] -> the kernel image at KERNEL_VBASE.
 *   - PML4[320] -> per-task kernel-stack vmalloc window (filled on demand).
 * There is deliberately NO identity map: PML4[0] (and the rest of the low
 * half) is left unmapped for user address spaces.
 *
 * Because every page-table page lives in physical RAM reachable through the
 * direct map, the table walker touches tables via phys_to_virt rather than
 * an identity alias. The bootstrap tables built in start.S already provide
 * the direct map, so this works from the first instruction we run high.
 */

#define PTE_ADDR   0x000FFFFFFFFFF000ULL   /* 4 KiB-aligned phys addr field */
#define ENTRIES    512
#define SZ_2MIB    0x200000ULL
#define SZ_1GIB    0x40000000ULL

#define PML4_IDX(va) (((uint64_t)(va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((uint64_t)(va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((uint64_t)(va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((uint64_t)(va) >> 12) & 0x1FF)

static uint64_t g_pml4_phys = 0;

/* Reach a page-table page (by physical address) through the direct map. */
static inline uint64_t *table(uint64_t phys) {
    return (uint64_t *)phys_to_virt(phys & PTE_ADDR);
}

static inline void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static uint64_t alloc_table(void) {
    uint64_t p = pmm_alloc_page();   /* PMM returns zeroed pages */
    if (!p) panic("paging: out of memory for page table");
    return p;
}

/* Descend one level, optionally creating the next table. `extra` is OR'd
   into the entry — used to propagate PTE_U so that every level on the path
   to a user page is user-accessible (the CPU ANDs the U bit across all
   levels, so the leaf still governs whether ring 3 actually gets in).
   Returns the next-level table, or nullptr if absent and create is false.
   Panics if it hits a large-page mapping. */
static uint64_t *descend(uint64_t *tbl, uint64_t idx, int create, uint64_t extra) {
    if (!(tbl[idx] & PTE_P)) {
        if (!create) return nullptr;
        uint64_t p = alloc_table();
        tbl[idx] = p | PTE_P | PTE_W | extra;
        return table(p);
    }
    if (tbl[idx] & PTE_PS)
        panic("paging: hit 2 MiB page where a table was expected");
    tbl[idx] |= extra;   /* upgrade a shared parent (e.g. add U) if needed */
    return table(tbl[idx] & PTE_ADDR);
}

void vmap(uint64_t va, uint64_t pa, size_t pages, uint64_t flags) {
    uint64_t inter = flags & PTE_U;   /* user pages need U on every level */
    uint64_t *pml4 = table(g_pml4_phys);
    for (size_t i = 0; i < pages; i++, va += PAGE_SIZE, pa += PAGE_SIZE) {
        uint64_t *pdpt = descend(pml4, PML4_IDX(va), 1, inter);
        uint64_t *pd   = descend(pdpt, PDPT_IDX(va), 1, inter);
        uint64_t *pt   = descend(pd,   PD_IDX(va),   1, inter);
        pt[PT_IDX(va)] = (pa & PTE_ADDR) | flags | PTE_P;
        invlpg(va);
    }
}

void vunmap(uint64_t va, size_t pages) {
    uint64_t *pml4 = table(g_pml4_phys);
    for (size_t i = 0; i < pages; i++, va += PAGE_SIZE) {
        uint64_t *pdpt = descend(pml4, PML4_IDX(va), 0, 0);
        if (!pdpt) continue;
        uint64_t *pd = descend(pdpt, PDPT_IDX(va), 0, 0);
        if (!pd) continue;
        uint64_t *pt = descend(pd, PD_IDX(va), 0, 0);
        if (!pt) continue;
        pt[PT_IDX(va)] = 0;
        invlpg(va);
    }
}

int paging_query(uint64_t va, uint64_t *pa_out) {
    uint64_t *pml4 = table(g_pml4_phys);
    if (!(pml4[PML4_IDX(va)] & PTE_P)) return 0;
    uint64_t *pdpt = table(pml4[PML4_IDX(va)] & PTE_ADDR);
    if (!(pdpt[PDPT_IDX(va)] & PTE_P)) return 0;
    uint64_t pdpte = pdpt[PDPT_IDX(va)];
    if (pdpte & PTE_PS) {            /* 1 GiB page (we never make these) */
        if (pa_out) *pa_out = (pdpte & PTE_ADDR) + (va & (SZ_1GIB - 1));
        return 1;
    }
    uint64_t *pd = table(pdpte & PTE_ADDR);
    if (!(pd[PD_IDX(va)] & PTE_P)) return 0;
    uint64_t pde = pd[PD_IDX(va)];
    if (pde & PTE_PS) {             /* 2 MiB page (identity / direct map) */
        if (pa_out) *pa_out = (pde & PTE_ADDR) + (va & (SZ_2MIB - 1));
        return 1;
    }
    uint64_t *pt = table(pde & PTE_ADDR);
    if (!(pt[PT_IDX(va)] & PTE_P)) return 0;
    if (pa_out) *pa_out = (pt[PT_IDX(va)] & PTE_ADDR) + (va & PAGE_MASK);
    return 1;
}

void paging_init(const BootInfo *bi) {
    /* How much physical memory to cover with the direct map. At least
       4 GiB, extended if the framebuffer (or kernel) sits higher so MMIO
       stays reachable. 2 MiB pages, one PD per GiB. */
    uint64_t fb_top  = bi->fb.base + bi->fb.size;
    uint64_t cover   = 4 * SZ_1GIB;
    while (fb_top > cover || bi->kernel_phys_end > cover) cover += SZ_1GIB;
    uint64_t ngib = cover / SZ_1GIB;

    uint64_t pml4_phys = alloc_table();
    uint64_t pdpt_dm   = alloc_table();   /* direct map  PML4[256] */
    uint64_t pdpt_kv   = alloc_table();   /* kernel high PML4[511] */

    uint64_t *pml4 = table(pml4_phys);
    uint64_t *pdm  = table(pdpt_dm);
    uint64_t *pkv  = table(pdpt_kv);

    /* Direct map: PHYS_OFFSET + phys, covering `ngib` GiB. */
    for (uint64_t g = 0; g < ngib; g++) {
        uint64_t pd_phys = alloc_table();
        uint64_t *pd = table(pd_phys);
        for (uint64_t i = 0; i < ENTRIES; i++)
            pd[i] = (g * SZ_1GIB + i * SZ_2MIB) | PTE_P | PTE_W | PTE_PS;
        pdm[g] = pd_phys | PTE_P | PTE_W;
    }

    /* Kernel image at KERNEL_VBASE (-2 GiB): one PD (1 GiB window) mapping
       the high VA back to physical 0..1 GiB. The image is tiny and sits at
       1 MiB physical, so a single GiB more than covers it. */
    uint64_t pd_kv_phys = alloc_table();
    uint64_t *pd_kv = table(pd_kv_phys);
    for (uint64_t i = 0; i < ENTRIES; i++)
        pd_kv[i] = (i * SZ_2MIB) | PTE_P | PTE_W | PTE_PS;
    pkv[PDPT_IDX(KERNEL_VBASE)] = pd_kv_phys | PTE_P | PTE_W;

    pml4[PML4_IDX(PHYS_OFFSET)]  = pdpt_dm | PTE_P | PTE_W;
    pml4[PML4_IDX(KERNEL_VBASE)] = pdpt_kv | PTE_P | PTE_W;
    /* PML4[0] and the rest of the low half are intentionally left unmapped
       — that is the address space userspace will own. */

    g_pml4_phys = pml4_phys;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");

    kprintf("[paging] CR3=%lx, kernel@%lx, direct map %lu GiB @ %lx (low half free)\n",
            (unsigned long)pml4_phys, (unsigned long)KERNEL_VBASE,
            (unsigned long)ngib, (unsigned long)PHYS_OFFSET);
}

/* Translate a kernel virtual address to its physical address by walking the
   active tables. Used where a physical address is required (e.g. virtio DMA
   descriptors), since kernel pointers are now direct-map / high-half VAs,
   never identity. Panics if `va` is not mapped. */
uint64_t kva_to_phys(const volatile void *va) {
    uint64_t pa;
    if (!paging_query((uint64_t)(uintptr_t)va, &pa))
        panic("kva_to_phys: %p not mapped", (void *)(uintptr_t)va);
    return pa;
}

uint64_t kstack_alloc(size_t pages, uint64_t *base_out) {
    static uint64_t next = KSTACK_REGION;

    uint64_t f = irq_save();
    uint64_t guard = next;                 /* unmapped page below stack */
    uint64_t base  = guard + PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) panic("kstack_alloc: out of physical memory");
        vmap(base + i * PAGE_SIZE, pa, 1, PTE_P | PTE_W);
    }
    uint64_t top = base + pages * PAGE_SIZE;
    next = top + PAGE_SIZE;                 /* trailing gap before next */
    irq_restore(f);

    if (base_out) *base_out = base;
    return top;
}
