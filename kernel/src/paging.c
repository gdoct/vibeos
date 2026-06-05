#include "kernel.h"
#include "pmm.h"
#include "irq.h"
#include "paging.h"
#include "kmalloc.h"
#include "smp.h"

/* Per-CPU record of the PML4 currently loaded in CR3, updated at every CR3 load
   for a task/vmspace. vmspace_destroy() consults it to refuse freeing a page-
   table tree that a CPU is still running on (a use-after-free that makes the
   kernel's own mapping vanish and triple-faults). */
static volatile uint64_t g_active_pml4[SMP_MAX_CPUS];
void paging_note_active(uint64_t pml4_phys) {
    int c = smp_cpu_index();
    if (c >= 0 && c < SMP_MAX_CPUS) g_active_pml4[c] = pml4_phys;
}
static int pml4_active_cpu(uint64_t pml4_phys) {
    for (int c = 0; c < SMP_MAX_CPUS; c++)
        if (g_active_pml4[c] == pml4_phys) return c;
    return -1;
}

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

/* Map/unmap/query against an explicit PML4 (physical). The public vmap/
   vunmap/paging_query operate on the kernel master tables; the vmspace_*
   helpers below reuse these against a process's own PML4. */
static void map_at(uint64_t pml4_phys, uint64_t va, uint64_t pa,
                   size_t pages, uint64_t flags) {
    uint64_t inter = flags & PTE_U;   /* user pages need U on every level */
    uint64_t *pml4 = table(pml4_phys);
    for (size_t i = 0; i < pages; i++, va += PAGE_SIZE, pa += PAGE_SIZE) {
        uint64_t *pdpt = descend(pml4, PML4_IDX(va), 1, inter);
        uint64_t *pd   = descend(pdpt, PDPT_IDX(va), 1, inter);
        uint64_t *pt   = descend(pd,   PD_IDX(va),   1, inter);
        pt[PT_IDX(va)] = (pa & PTE_ADDR) | flags | PTE_P;
        invlpg(va);
    }
}

static void unmap_at(uint64_t pml4_phys, uint64_t va, size_t pages) {
    uint64_t *pml4 = table(pml4_phys);
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

static int query_at(uint64_t pml4_phys, uint64_t va, uint64_t *pa_out) {
    uint64_t *pml4 = table(pml4_phys);
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

/* Return a pointer to the existing 4 KiB leaf PTE for `va` in `pml4_phys`, or
   nullptr if any level along the way is absent (or a large page). The pointer
   lives in the direct map, so writes to it edit the live table in place. */
static uint64_t *leaf_pte(uint64_t pml4_phys, uint64_t va) {
    uint64_t *pml4 = table(pml4_phys);
    if (!(pml4[PML4_IDX(va)] & PTE_P)) return nullptr;
    uint64_t *pdpt = table(pml4[PML4_IDX(va)] & PTE_ADDR);
    if (!(pdpt[PDPT_IDX(va)] & PTE_P) || (pdpt[PDPT_IDX(va)] & PTE_PS)) return nullptr;
    uint64_t *pd = table(pdpt[PDPT_IDX(va)] & PTE_ADDR);
    if (!(pd[PD_IDX(va)] & PTE_P) || (pd[PD_IDX(va)] & PTE_PS)) return nullptr;
    uint64_t *pt = table(pd[PD_IDX(va)] & PTE_ADDR);
    if (!(pt[PT_IDX(va)] & PTE_P)) return nullptr;
    return &pt[PT_IDX(va)];
}

void vmap(uint64_t va, uint64_t pa, size_t pages, uint64_t flags) {
    map_at(g_pml4_phys, va, pa, pages, flags);
}
void vunmap(uint64_t va, size_t pages) { unmap_at(g_pml4_phys, va, pages); }
int  paging_query(uint64_t va, uint64_t *pa_out) {
    return query_at(g_pml4_phys, va, pa_out);
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

    /* Pre-create the kernel-stack region's top-level PDPT so every per-process
       address space can share it by copying this one PML4 entry. Kernel stacks
       allocated later (kstack_alloc) populate the shared subtree and stay
       visible in all address spaces — essential, since a process traps into
       the kernel onto its own kernel stack while its CR3 is active. */
    pml4[PML4_IDX(KSTACK_REGION)] = alloc_table() | PTE_P | PTE_W;

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

/* ==== Page refcounts + copy-on-write (ROADMAP §1.1) ======================= */

/* refcnt[i] = (number of PTEs pointing at arena page i) - 1. A page mapped by a
   single PTE therefore reads as 0 (the zero-initialised default), so ordinary
   private mappings need no bookkeeping; only fork()'s sharing touches this. */
static uint16_t *g_refcnt = nullptr;
static uint64_t  g_rc_base  = 0;     /* arena base phys */
static uint64_t  g_rc_pages = 0;

void page_refcount_init(void) {
    g_rc_base  = pmm_arena_base();
    g_rc_pages = pmm_arena_pages();
    uint64_t bytes = g_rc_pages * sizeof(uint16_t);
    uint64_t need  = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    uint64_t phys  = pmm_alloc_pages(need);     /* zeroed by the PMM */
    if (!phys) panic("page_refcount_init: out of memory for %lu pages",
                     (unsigned long)need);
    g_refcnt = (uint16_t *)phys_to_virt(phys);
    kprintf("[paging] page refcounts: %lu arena pages, %lu KiB table\n",
            (unsigned long)g_rc_pages, (unsigned long)(need * PAGE_SIZE >> 10));
}

static inline int rc_index(uint64_t pa, uint64_t *idx) {
    if (!g_refcnt || pa < g_rc_base) return 0;
    uint64_t i = (pa - g_rc_base) >> 12;
    if (i >= g_rc_pages) return 0;
    *idx = i;
    return 1;
}

/* Atomically decrement g_refcnt[i] only while it is > 0; returns 1 if it
   decremented (the page is still shared by someone else), 0 if it was already 0
   (the caller is the sole owner). The CAS loop makes the read-test-decrement one
   indivisible step, so two cores dropping/COWing the same shared page can't both
   take the "still shared" branch and underflow the count. */
static int rc_dec_if_positive(uint64_t i) {
    uint16_t cur = __atomic_load_n(&g_refcnt[i], __ATOMIC_ACQUIRE);
    while (cur > 0) {
        if (__atomic_compare_exchange_n(&g_refcnt[i], &cur, (uint16_t)(cur - 1),
                                        true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
    }
    return 0;
}

/* Add a shared reference to `pa` (called when a second PTE starts pointing at
   it, e.g. fork sharing). Pages outside the arena (none, in practice) are
   silently ignored. */
static void page_ref(uint64_t pa) {
    uint64_t i;
    if (rc_index(pa, &i)) __atomic_add_fetch(&g_refcnt[i], 1, __ATOMIC_ACQ_REL);
}

void paging_unref_page(uint64_t pa) {
    uint64_t i;
    if (rc_index(pa, &i) && rc_dec_if_positive(i)) return;  /* still shared */
    pmm_free_page(pa);                          /* last (or untracked) reference */
}

int paging_cow_fault(vmspace_t *vm, uint64_t va) {
    va &= ~PAGE_MASK;
    /* Serialize repair of this address space's tables so two threads faulting the
       same (or neighbouring) page on different cores can't both rewrite the PTE
       off a stale read. The PTE is re-read under the lock. */
    spin_lock(&vm->lock);
    uint64_t *pte = leaf_pte(vm->pml4_phys, va);
    if (!pte) { spin_unlock(&vm->lock); return 0; }
    uint64_t e = *pte;
    if (!(e & PTE_U) || !(e & PTE_COW)) {           /* already repaired / not COW */
        spin_unlock(&vm->lock);
        return (e & PTE_W) ? 1 : 0;                 /* another core fixed it: retry */
    }

    uint64_t old = e & PTE_ADDR;
    uint64_t i;
    int shared = rc_index(old, &i) &&
                 __atomic_load_n(&g_refcnt[i], __ATOMIC_ACQUIRE) > 0;
    if (shared) {                                   /* possibly shared: private copy */
        uint64_t neu = pmm_alloc_page();
        if (!neu) { spin_unlock(&vm->lock); return 0; }  /* OOM: caller kills task */
        kmemcpy(phys_to_virt(neu), phys_to_virt(old), PAGE_SIZE);
        if (rc_dec_if_positive(i)) {                /* won the drop: keep the copy */
            *pte = (neu & PTE_ADDR) | (e & ~PTE_ADDR & ~PTE_COW) | PTE_W;
        } else {                                    /* lost the race: now sole owner */
            pmm_free_page(neu);
            *pte = (e & ~PTE_COW) | PTE_W;
        }
    } else {                                        /* sole owner: just re-grant W */
        *pte = (e & ~PTE_COW) | PTE_W;
    }
    invlpg(va);
    spin_unlock(&vm->lock);
    return 1;
}

/* ---- validated user/kernel copies ---- */

/* Resolve user page `uva` in `vm` to its physical address, ensuring access.
   For writes, break COW so the page is privately writable. Returns 0 on
   success with *pa_out set, -1 on any access violation. */
static int user_page_pa(vmspace_t *vm, uint64_t uva, int write, uint64_t *pa_out) {
    uva &= ~PAGE_MASK;
    uint64_t *pte = leaf_pte(vm->pml4_phys, uva);
    if (!pte || !(*pte & PTE_U)) return -1;
    if (write && !(*pte & PTE_W)) {
        if (!paging_cow_fault(vm, uva)) return -1;  /* RO and not COW: refuse */
        pte = leaf_pte(vm->pml4_phys, uva);
        if (!pte || !(*pte & PTE_W)) return -1;
    }
    *pa_out = *pte & PTE_ADDR;
    return 0;
}

/* Range entirely inside the user half and non-wrapping. */
static int user_range_ok(uint64_t addr, uint64_t n) {
    if (n == 0) return 1;
    if (addr >= USER_ADDR_END) return 0;
    if (addr + n < addr) return 0;                  /* wrap */
    if (addr + n > USER_ADDR_END) return 0;
    return 1;
}

long copy_from_user(void *kdst, vmspace_t *vm, uint64_t usrc, uint64_t n) {
    if (!user_range_ok(usrc, n)) return -1;
    uint8_t *d = (uint8_t *)kdst;
    while (n) {
        uint64_t pa, off = usrc & PAGE_MASK;
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        if (user_page_pa(vm, usrc, 0, &pa) < 0) return -1;
        kmemcpy(d, (uint8_t *)phys_to_virt(pa) + off, chunk);
        d += chunk; usrc += chunk; n -= chunk;
    }
    return 0;
}

long copy_to_user(vmspace_t *vm, uint64_t udst, const void *ksrc, uint64_t n) {
    if (!user_range_ok(udst, n)) return -1;
    const uint8_t *s = (const uint8_t *)ksrc;
    while (n) {
        uint64_t pa, off = udst & PAGE_MASK;
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        if (user_page_pa(vm, udst, 1, &pa) < 0) return -1;
        kmemcpy((uint8_t *)phys_to_virt(pa) + off, s, chunk);
        s += chunk; udst += chunk; n -= chunk;
    }
    return 0;
}

int paging_user_ok(vmspace_t *vm, uint64_t addr, uint64_t n, int write) {
    if (!user_range_ok(addr, n)) return 0;
    if (n == 0) return 1;
    uint64_t a = addr & ~PAGE_MASK;
    uint64_t end = addr + n;
    for (; a < end; a += PAGE_SIZE) {
        uint64_t pa;
        if (user_page_pa(vm, a, write, &pa) < 0) return 0;
    }
    return 1;
}

long strncpy_from_user(char *kdst, vmspace_t *vm, uint64_t usrc, uint64_t max) {
    if (max == 0) return -2;
    for (uint64_t i = 0; i < max; i++) {
        if (usrc + i >= USER_ADDR_END) return -1;
        uint64_t pa;
        if (user_page_pa(vm, usrc + i, 0, &pa) < 0) return -1;
        char c = *((char *)phys_to_virt(pa) + ((usrc + i) & PAGE_MASK));
        kdst[i] = c;
        if (c == '\0') return (long)i;
    }
    kdst[max - 1] = '\0';
    return -2;                                       /* truncated */
}

/* ==== Per-process address spaces (ROADMAP §3 Milestone B) ================= */

uint64_t paging_kernel_pml4(void) { return g_pml4_phys; }

/* A fresh address space: a new PML4 whose upper (kernel) half is shared with
   the master tables by copying the top-level entries, and whose lower half is
   empty for this process's own user mappings. */
vmspace_t *vmspace_create(void) {
    vmspace_t *vm = (vmspace_t *)kmalloc(sizeof(vmspace_t));
    if (!vm) return nullptr;
    uint64_t p = alloc_table();                 /* zeroed PML4 */
    uint64_t *npml4 = table(p);
    uint64_t *kpml4 = table(g_pml4_phys);
    for (int i = 256; i < ENTRIES; i++) npml4[i] = kpml4[i];   /* share kernel half */
    vm->pml4_phys = p;
    vm->mmap_next = 0;                           /* set by the loader (elf64) */
    vm->ref = 1;
    spin_lock_init(&vm->lock);
    return vm;
}

void vmspace_ref(vmspace_t *vm) { if (vm) __atomic_add_fetch(&vm->ref, 1, __ATOMIC_ACQ_REL); }

void vmspace_switch(vmspace_t *vm) {
    uint64_t cr3 = vm ? vm->pml4_phys : g_pml4_phys;
    paging_note_active(cr3);
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

void vmspace_map(vmspace_t *vm, uint64_t va, uint64_t pa, size_t pages, uint64_t flags) {
    spin_lock(&vm->lock);
    map_at(vm->pml4_phys, va, pa, pages, flags);
    spin_unlock(&vm->lock);
}

int vmspace_query(vmspace_t *vm, uint64_t va, uint64_t *pa_out) {
    return query_at(vm->pml4_phys, va, pa_out);
}

void vmspace_unmap(vmspace_t *vm, uint64_t va, size_t pages) {
    spin_lock(&vm->lock);
    unmap_at(vm->pml4_phys, va, pages);
    spin_unlock(&vm->lock);
}

/* Copy-on-write fork: share the parent's user pages with the child read-only
   instead of deep-copying. Every shared writable leaf is demoted to read-only
   with PTE_COW set in BOTH address spaces and its refcount bumped; the first
   write in either process triggers paging_cow_fault to make a private copy.
   Read-only pages (e.g. text) are shared as-is — still refcounted so neither
   destroy double-frees them. */
vmspace_t *vmspace_fork(vmspace_t *parent) {
    vmspace_t *child = vmspace_create();
    if (!child) return nullptr;
    child->mmap_next = parent->mmap_next;       /* inherit the mmap arena position */
    /* Lock the parent across the walk: it demotes the parent's writable PTEs to
       COW, which must not race a sibling thread's COW fault on the same core's
       address space. The child is still private (ref==1), so it needs no lock. */
    spin_lock(&parent->lock);
    uint64_t *ppml4 = table(parent->pml4_phys);
    for (uint64_t i = 0; i < 256; i++) {
        if (!(ppml4[i] & PTE_P)) continue;
        uint64_t *ppdpt = table(ppml4[i] & PTE_ADDR);
        for (uint64_t j = 0; j < ENTRIES; j++) {
            if (!(ppdpt[j] & PTE_P) || (ppdpt[j] & PTE_PS)) continue;
            uint64_t *ppd = table(ppdpt[j] & PTE_ADDR);
            for (uint64_t k = 0; k < ENTRIES; k++) {
                if (!(ppd[k] & PTE_P) || (ppd[k] & PTE_PS)) continue;
                uint64_t *ppt = table(ppd[k] & PTE_ADDR);
                for (uint64_t l = 0; l < ENTRIES; l++) {
                    uint64_t pte = ppt[l];
                    if (!(pte & PTE_P)) continue;
                    uint64_t va = (i << 39) | (j << 30) | (k << 21) | (l << 12);
                    uint64_t pa = pte & PTE_ADDR;
                    if (pte & PTE_W) {                 /* demote writable -> COW */
                        pte = (pte & ~PTE_W) | PTE_COW;
                        ppt[l] = pte;                  /* parent now shares it RO */
                    }
                    map_at(child->pml4_phys, va, pa, 1, pte & ~PTE_ADDR);
                    page_ref(pa);                      /* +1 sharer */
                }
            }
        }
    }
    /* Flush this core's TLB: the parent's writable pages just became read-only
       COW (parent is the active address space during fork). If the parent is
       multithreaded, sibling threads on other cores hold stale writable entries;
       the fork syscall issues a cross-core shootdown after this returns. */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    spin_unlock(&parent->lock);
    return child;
}

/* Free the user half (data pages + low-half page tables) and the PML4 itself.
   Leaf data pages are released through the refcount (so COW-shared pages held
   by another address space survive); page-table pages are private and freed
   outright. The kernel upper half is shared, so it is left untouched. The
   caller must not be running on this address space. */
void vmspace_destroy(vmspace_t *vm) {
    if (!vm) return;
    if (__atomic_sub_fetch(&vm->ref, 1, __ATOMIC_ACQ_REL) > 0)
        return;                         /* still shared by another thread */
    int busy = pml4_active_cpu(vm->pml4_phys);
    if (busy >= 0) {
        /* A CPU still has this PML4 in CR3 — freeing it now would pull the page
           tables out from under a running CPU and triple-fault. Leak it instead
           and shout, so the offending path is visible rather than fatal. */
        kprintf("[paging] BUG: vmspace pml4=%lx freed while active on CPU %d "
                "(cur cpu %d) -> leaking to avoid use-after-free\n",
                (unsigned long)vm->pml4_phys, busy, smp_cpu_index());
        return;
    }
    uint64_t *pml4 = table(vm->pml4_phys);
    for (uint64_t i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_P)) continue;
        uint64_t pdpt_pa = pml4[i] & PTE_ADDR; uint64_t *pdpt = table(pdpt_pa);
        for (uint64_t j = 0; j < ENTRIES; j++) {
            if (!(pdpt[j] & PTE_P) || (pdpt[j] & PTE_PS)) continue;
            uint64_t pd_pa = pdpt[j] & PTE_ADDR; uint64_t *pd = table(pd_pa);
            for (uint64_t k = 0; k < ENTRIES; k++) {
                if (!(pd[k] & PTE_P) || (pd[k] & PTE_PS)) continue;
                uint64_t pt_pa = pd[k] & PTE_ADDR; uint64_t *pt = table(pt_pa);
                for (uint64_t l = 0; l < ENTRIES; l++)
                    if (pt[l] & PTE_P) paging_unref_page(pt[l] & PTE_ADDR);
                pmm_free_page(pt_pa);
            }
            pmm_free_page(pd_pa);
        }
        pmm_free_page(pdpt_pa);
    }
    pmm_free_page(vm->pml4_phys);
    kfree(vm);
}
