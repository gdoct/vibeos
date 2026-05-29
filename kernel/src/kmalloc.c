#include "kernel.h"
#include "pmm.h"
#include "irq.h"
#include "spinlock.h"
#include "paging.h"   /* PMM hands out physical addrs; reach them via direct map */
#include "kmalloc.h"

static spinlock_t kmalloc_lock = SPINLOCK_INIT;

/*
 * A small heap on top of the physical page allocator.
 *
 * Every allocation is prefixed by a 16-byte header (which also keeps the
 * returned payload 16-byte aligned). For small requests we carve a PMM
 * page into equal power-of-two "chunks" and keep a freelist per size
 * class; freeing a chunk just pushes it back. Requests too big for the
 * largest class are rounded up to whole pages and backed directly by the
 * page allocator.
 *
 * Single-threaded-simple: every entry point runs under irq_save so it is
 * safe to call from task or (in principle) IRQ context.
 */

#define HDR_SIZE      16u
#define ALLOC_MAGIC   0x6B6D4C42u    /* "kmLB" */
#define CLASS_LARGE   0xFFFFu

/* Chunk sizes include the header. Payload available = chunk - HDR_SIZE. */
static const uint32_t k_class_size[] = {
    32, 64, 128, 256, 512, 1024, 2048,
};
#define NCLASS  (sizeof(k_class_size) / sizeof(k_class_size[0]))

typedef struct alloc_hdr {
    uint32_t magic;
    uint16_t class_idx;     /* index into k_class_size, or CLASS_LARGE */
    uint16_t _pad;
    uint64_t req_size;      /* original requested payload size */
} alloc_hdr_t;

static uint64_t pages_for(uint64_t req_size) {
    return (req_size + HDR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
}

typedef struct free_node {
    struct free_node *next;
} free_node_t;

static free_node_t *k_slabs[NCLASS];
static size_t       k_in_use;       /* payload bytes outstanding */

static int size_to_class(size_t need) {
    for (size_t i = 0; i < NCLASS; i++)
        if (need <= k_class_size[i]) return (int)i;
    return -1;
}

/* Carve a fresh page into chunks of class `c` and push them all onto the
   freelist. Returns 0 on success, -1 if out of memory. */
static int refill(int c) {
    uint64_t page = pmm_alloc_page();
    if (!page) return -1;
    uint32_t chunk = k_class_size[c];
    for (uint32_t off = 0; off + chunk <= PAGE_SIZE; off += chunk) {
        free_node_t *n = (free_node_t *)phys_to_virt(page + off);
        n->next = k_slabs[c];
        k_slabs[c] = n;
    }
    return 0;
}

void *kmalloc(size_t size) {
    if (size == 0) return nullptr;

    spin_lock(&kmalloc_lock);
    size_t need = size + HDR_SIZE;
    void *ret = nullptr;

    int c = size_to_class(need);
    if (c >= 0) {
        if (!k_slabs[c] && refill(c) < 0) { spin_unlock(&kmalloc_lock); return nullptr; }
        free_node_t *n = k_slabs[c];
        k_slabs[c] = n->next;

        alloc_hdr_t *h = (alloc_hdr_t *)n;
        h->magic     = ALLOC_MAGIC;
        h->class_idx = (uint16_t)c;
        h->req_size  = size;
        ret = (uint8_t *)h + HDR_SIZE;
    } else {
        uint64_t base = pmm_alloc_pages((size_t)pages_for(size));
        if (base) {
            alloc_hdr_t *h = (alloc_hdr_t *)phys_to_virt(base);
            h->magic     = ALLOC_MAGIC;
            h->class_idx = CLASS_LARGE;
            h->req_size  = size;
            ret = (uint8_t *)h + HDR_SIZE;
        }
    }

    if (ret) k_in_use += size;
    spin_unlock(&kmalloc_lock);
    return ret;
}

void kfree(void *ptr) {
    if (!ptr) return;

    alloc_hdr_t *h = (alloc_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    if (h->magic != ALLOC_MAGIC)
        panic("kfree: bad magic %x at %p (double free?)", h->magic, ptr);

    spin_lock(&kmalloc_lock);
    h->magic = 0;   /* poison so a double free trips the check above */

    k_in_use -= h->req_size;

    if (h->class_idx == CLASS_LARGE) {
        uint64_t base  = virt_to_phys(h);   /* PMM wants the physical address */
        uint64_t pages = pages_for(h->req_size);
        for (uint64_t i = 0; i < pages; i++)
            pmm_free_page(base + i * PAGE_SIZE);
    } else {
        int c = (int)h->class_idx;
        free_node_t *n = (free_node_t *)h;
        n->next = k_slabs[c];
        k_slabs[c] = n;
    }
    spin_unlock(&kmalloc_lock);
}

size_t kmalloc_in_use(void) { return k_in_use; }
