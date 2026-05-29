#include "kernel.h"
#include "pmm.h"
#include "irq.h"
#include "paging.h"   /* phys_to_virt: pages are reached via the direct map */

/* EFI memory type we treat as free. The full UEFI spec lists a few more
   that become free after ExitBootServices, but ConventionalMemory alone
   gives us plenty of headroom for day one. */
#define EFI_CONVENTIONAL_MEMORY 7

static uint64_t pmm_base   = 0;   /* start of bump arena */
static uint64_t pmm_cursor = 0;   /* next free byte */
static uint64_t pmm_end    = 0;   /* one past end of arena */
static uint64_t pmm_total  = 0;   /* total free bytes we saw at init */

/* Singly-linked freelist of returned single pages. Each free page stores
   the physical address of the next free page in its first 8 bytes, so the
   list costs no extra memory. Freed pages are reused before the bump
   cursor advances again. */
static uint64_t pmm_freelist = 0;   /* 0 = empty (phys 0 is never in arena) */
static uint64_t pmm_freed    = 0;   /* count of pages currently on freelist */

void pmm_init(const BootInfo *bi) {
    const uint8_t *p   = (const uint8_t *)phys_to_virt(bi->mmap.buffer);
    const uint8_t *end = p + bi->mmap.size;
    uint64_t dsz       = bi->mmap.desc_size;

    uint64_t kend = PAGE_ALIGN_UP(bi->kernel_phys_end);

    /* Pick the largest conventional region above the kernel. */
    uint64_t best_base = 0, best_len = 0;
    for (; p + dsz <= end; p += dsz) {
        const MemoryDescriptor *d = (const MemoryDescriptor *)p;
        if (d->type != EFI_CONVENTIONAL_MEMORY) continue;

        uint64_t r_lo = d->phys_start;
        uint64_t r_hi = r_lo + d->num_pages * PAGE_SIZE;
        if (r_hi <= kend) continue;
        if (r_lo < kend) r_lo = kend;
        uint64_t len = r_hi - r_lo;

        pmm_total += d->num_pages * PAGE_SIZE;

        if (len > best_len) { best_base = r_lo; best_len = len; }
    }

    if (best_len == 0) panic("pmm: no conventional memory above kernel");

    pmm_base   = best_base;
    pmm_cursor = best_base;
    pmm_end    = best_base + best_len;

    kprintf("[pmm] arena %lx..%lx (%lu MiB), total free %lu MiB\n",
            (unsigned long)pmm_base, (unsigned long)pmm_end,
            (unsigned long)(best_len >> 20),
            (unsigned long)(pmm_total >> 20));
}

uint64_t pmm_alloc_pages(size_t pages) {
    if (pages == 0) return 0;

    uint64_t f = irq_save();

    /* Single-page requests prefer the freelist so returned pages get
       reused. Multi-page requests need contiguity, which the freelist
       can't promise, so they always come from the bump arena. */
    if (pages == 1 && pmm_freelist) {
        uint64_t addr = pmm_freelist;
        pmm_freelist = *(uint64_t *)phys_to_virt(addr);   /* next-free link */
        pmm_freed--;
        irq_restore(f);
        kmemset(phys_to_virt(addr), 0, PAGE_SIZE);
        return addr;
    }

    uint64_t bytes = (uint64_t)pages * PAGE_SIZE;
    if (pmm_cursor + bytes > pmm_end) { irq_restore(f); return 0; }
    uint64_t addr = pmm_cursor;
    pmm_cursor += bytes;
    irq_restore(f);
    kmemset(phys_to_virt(addr), 0, (size_t)bytes);
    return addr;
}

uint64_t pmm_alloc_page(void) { return pmm_alloc_pages(1); }

void pmm_free_page(uint64_t phys) {
    if (phys == 0) return;
    if (phys & PAGE_MASK) panic("pmm_free_page: %lx not page-aligned",
                                (unsigned long)phys);
    uint64_t f = irq_save();
    *(uint64_t *)phys_to_virt(phys) = pmm_freelist;   /* store next-free link */
    pmm_freelist = phys;
    pmm_freed++;
    irq_restore(f);
}

uint64_t pmm_free_bytes(void)  {
    return (pmm_end - pmm_cursor) + pmm_freed * PAGE_SIZE;
}
uint64_t pmm_total_bytes(void) { return pmm_total; }
