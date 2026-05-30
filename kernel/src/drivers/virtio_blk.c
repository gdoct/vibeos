#include "kernel.h"
#include "io.h"
#include "pmm.h"
#include "paging.h"   /* phys_to_virt for CPU access, kva_to_phys for DMA */
#include "pci.h"
#include "block.h"
#include "virtio.h"
#include "irq.h"
#include "apic.h"
#include "task.h"

/*
 * Legacy virtio-blk over PCI IO BAR0.
 *
 * Why legacy: it lets us drive a real disk in <300 lines without the PCI
 * capability walk, MMIO mapping, and 64-bit feature negotiation that the
 * modern interface demands. QEMU's transitional virtio-blk-pci device
 * stays in legacy mode as long as we don't ack VIRTIO_F_VERSION_1.
 *
 * We run a single virtqueue (the only one virtio-blk has) and poll for
 * completion — no interrupts yet. Every request uses descriptors 0, 1,
 * 2 of the chain (header / data / status), which is fine because we
 * issue exactly one request at a time.
 */

typedef struct virtio_blk {
    block_device_t bd;

    uint16_t io;            /* BAR0 IO port base */
    uint16_t qsize;         /* queue size the device exposes */

    virtq_desc_t       *desc;
    uint16_t           *avail;        /* points at avail.flags */
    uint16_t           *avail_ring;   /* avail.ring[0..qsize-1] */
    uint16_t           *avail_idx;    /* &avail.idx */
    uint16_t           *used;         /* points at used.flags */
    uint16_t           *used_idx;     /* &used.idx */
    virtq_used_elem_t  *used_ring;

    volatile uint16_t last_used_idx;  /* what we've already observed */

    uint8_t    irq_line;              /* PIC IRQ from PCI config 0x3C */
    uint8_t    irq_ok;                /* completion IRQ wired (block vs poll) */
    wait_queue_t wq;                  /* submitter blocks here for completion */
    volatile uint64_t completions;    /* total requests the IRQ has reaped */
    volatile int busy;                /* a request is in flight (§2: serialize submitters) */
    wait_queue_t busy_wq;             /* submitters queue here for the device */

    /* Per-request scratch: header and trailing status byte. Static so
       their physical address is stable across calls and we can hand it
       to the device. */
    virtio_blk_req_hdr_t hdr;
    volatile uint8_t     status;
} virtio_blk_t;

static virtio_blk_t g_vblk;

/* Advance our view of the used ring up to the device's used.idx. With a
   single in-flight request the only thing that matters is reaching the
   index we're waiting on; the status byte the device wrote is already in
   v->status. Caller holds interrupts off. */
static void vblk_reap(virtio_blk_t *v) {
    while (v->last_used_idx != *v->used_idx) {
        v->last_used_idx++;
        v->completions++;
    }
}

/* INTx handler. The virtio ISR register at offset 0x13 is clear-on-read;
   reading it both tells us the device raised the interrupt and deasserts
   the (level-triggered) PCI line, so we read it before returning. */
static void vblk_irq(uint8_t irq, regs_t *regs) {
    (void)irq; (void)regs;
    uint8_t isr = inb(g_vblk.io + VIRTIO_PCI_ISR);
    if (!(isr & 0x1)) return;       /* not ours / no buffers used */
    /* Reap and wake under sched_lock: the submitter may be sleeping on another
       CPU, and it checks last_used_idx under the same lock (ROADMAP §2), so the
       reap (which advances last_used_idx) and the wake are atomic w.r.t. its
       check — no lost wakeup. */
    sched_lock();
    vblk_reap(&g_vblk);
    wait_queue_wake_all_locked(&g_vblk.wq);
    sched_unlock();
}

/* ---- low-level IO ---- */

static inline void cmd_status(virtio_blk_t *v, uint8_t bits) {
    outb(v->io + VIRTIO_PCI_STATUS, bits);
}

static inline uint32_t pci_bar0_io(pci_dev_t *d) {
    uint32_t bar = pci_read32(d->bus, d->dev, d->fn, PCI_BAR0);
    if ((bar & 1) == 0) panic("virtio-blk: BAR0 is MMIO, expected IO");
    return bar & ~0x3u;
}

static void pci_enable(pci_dev_t *d) {
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, PCI_COMMAND);
    cmd |= PCI_CMD_IO | PCI_CMD_BUS_MASTER;
    pci_write16(d->bus, d->dev, d->fn, PCI_COMMAND, cmd);
}

/* ---- virtqueue layout ---- */

static void virtq_setup(virtio_blk_t *v) {
    /* Select queue 0, read its size. */
    outw(v->io + VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t n = inw(v->io + VIRTIO_PCI_QUEUE_NUM);
    if (n == 0) panic("virtio-blk: queue 0 not present");
    if (n > 256) n = 256;   /* cap to keep the allocation bounded */
    v->qsize = n;

    /* Layout per legacy virtio:
         desc[N]                       16*N
         avail (flags,idx,ring[N],_)   6 + 2*N
         (pad to PAGE_SIZE)
         used  (flags,idx,ring[N],_)   6 + 8*N
    */
    uint64_t desc_bytes  = (uint64_t)16 * n;
    uint64_t avail_bytes = 6 + 2 * (uint64_t)n;
    uint64_t pre_used    = desc_bytes + avail_bytes;
    uint64_t used_off    = PAGE_ALIGN_UP(pre_used);
    uint64_t used_bytes  = 6 + 8 * (uint64_t)n;
    uint64_t total       = PAGE_ALIGN_UP(used_off + used_bytes);

    uint64_t phys = pmm_alloc_pages(total / PAGE_SIZE);
    if (!phys) panic("virtio-blk: out of memory for virtqueue");

    uint8_t *base = (uint8_t *)phys_to_virt(phys);   /* CPU view of the rings */
    v->desc       = (virtq_desc_t *)base;
    v->avail      = (uint16_t *)(base + desc_bytes);          /* &flags */
    v->avail_idx  = v->avail + 1;
    v->avail_ring = v->avail + 2;
    v->used       = (uint16_t *)(base + used_off);
    v->used_idx   = v->used + 1;
    v->used_ring  = (virtq_used_elem_t *)(base + used_off + 4);
    v->last_used_idx = 0;

    /* Hand the device the page frame number of the desc table. */
    outl(v->io + VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys / PAGE_SIZE));
}

/* ---- request submission ---- */

static int submit(virtio_blk_t *v, uint32_t type, uint64_t lba,
                  uint32_t count, void *buf, int dev_writes_data) {
    uint32_t data_bytes = count * 512u;

    /* Only one request may use the single descriptor chain / hdr / status at a
       time. With user tasks on every core (ROADMAP §2), several processes can
       enter here at once, so claim the device first and let others queue. */
    int serialize = sched_active();
    if (serialize) {
        sched_lock();
        while (v->busy) wait_queue_sleep_locked(&v->busy_wq);
        v->busy = 1;
        sched_unlock();
    }

    v->hdr.type     = type;
    v->hdr.reserved = 0;
    v->hdr.sector   = lba;
    v->status       = 0xFF;     /* sentinel; device overwrites */

    /* Descriptor addresses are what the device DMAs against, so they must be
       physical. Kernel pointers are direct-map / high-half VAs, never
       identity — translate via the page tables (kva_to_phys). */

    /* desc[0]: header (read-only for device) */
    v->desc[0].addr  = kva_to_phys(&v->hdr);
    v->desc[0].len   = sizeof(v->hdr);
    v->desc[0].flags = VIRTQ_DESC_F_NEXT;
    v->desc[0].next  = 1;

    /* Data buffer, scatter-gathered per physical page. The caller's buffer may
       be a kernel stack / vmalloc region whose pages are NOT physically
       contiguous, so a single (addr,len) would DMA across a page boundary into
       the wrong frame. Emit one descriptor per page-bounded chunk, translating
       each independently. */
    uint16_t d = 1;
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = data_bytes;
    uint16_t data_flag = VIRTQ_DESC_F_NEXT | (dev_writes_data ? VIRTQ_DESC_F_WRITE : 0);
    while (remaining > 0) {
        uint32_t page_off = (uint32_t)((uintptr_t)p & PAGE_MASK);
        uint32_t chunk = (uint32_t)PAGE_SIZE - page_off;
        if (chunk > remaining) chunk = remaining;
        v->desc[d].addr  = kva_to_phys(p);
        v->desc[d].len   = chunk;
        v->desc[d].flags = data_flag;
        v->desc[d].next  = (uint16_t)(d + 1);
        p += chunk; remaining -= chunk; d++;
        if (d >= v->qsize - 1)
            panic("virtio-blk: transfer too large (%u bytes)", data_bytes);
    }

    /* status byte (device-write), terminating the chain */
    v->desc[d].addr  = kva_to_phys(&v->status);
    v->desc[d].len   = 1;
    v->desc[d].flags = VIRTQ_DESC_F_WRITE;
    v->desc[d].next  = 0;

    /* Add to available ring. x86 TSO + compiler barrier is enough. */
    uint16_t idx = *v->avail_idx;
    v->avail_ring[idx % v->qsize] = 0;          /* head desc */
    __asm__ volatile("" ::: "memory");
    *v->avail_idx = idx + 1;
    __asm__ volatile("" ::: "memory");

    uint16_t want = v->last_used_idx + 1;

    /* Notify queue 0. */
    outw(v->io + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Wait for the device to mark our chain used. Once the scheduler is
       running we block on a wait queue and let vblk_irq wake us — so
       another task gets the CPU while the disk works. Before the
       scheduler exists (early-boot self-tests), there's nothing to switch
       to, so fall back to polling and reaping inline. */
    if (sched_active() && v->irq_ok) {
        /* Check completion and sleep under sched_lock — vblk_irq reaps + wakes
           under the same lock, so a completion on another CPU can't slip
           between our check and our sleep (ROADMAP §2). */
        sched_lock();
        while (v->last_used_idx != want)
            wait_queue_sleep_locked(&v->wq);
        sched_unlock();
    } else {
        while (v->last_used_idx != want) {
            __asm__ volatile("pause" ::: "memory");
            vblk_reap(v);
        }
    }

    int rc = (v->status == VIRTIO_BLK_S_OK) ? BLK_OK : BLK_ERR_IO;

    if (serialize) {                    /* release the device, wake the next submitter */
        sched_lock();
        v->busy = 0;
        wait_queue_wake_one_locked(&v->busy_wq);
        sched_unlock();
    }
    return rc;
}

extern "C" uint64_t virtio_blk_completions(void) { return g_vblk.completions; }

static int vblk_read(block_device_t *bd, uint64_t lba, uint32_t count, void *buf) {
    virtio_blk_t *v = (virtio_blk_t *)bd;
    if (lba + count > bd->num_blocks) return BLK_ERR_RANGE;
    return submit(v, VIRTIO_BLK_T_IN, lba, count, buf, 1);
}

static int vblk_write(block_device_t *bd, uint64_t lba, uint32_t count, const void *buf) {
    virtio_blk_t *v = (virtio_blk_t *)bd;
    if (lba + count > bd->num_blocks) return BLK_ERR_RANGE;
    return submit(v, VIRTIO_BLK_T_OUT, lba, count, (void *)buf, 0);
}

/* ---- public init ---- */

extern "C" block_device_t *virtio_blk_init(void) {
    pci_dev_t d;
    /* Match the block subsystem specifically so we don't grab the NIC (both are
       virtio, same device-id range) — ROADMAP §5. */
    if (!pci_find_subsys(VIRTIO_PCI_VENDOR, VIRTIO_SUBSYSTEM_BLOCK, &d)) {
        kprintf("[virtio-blk] no device found\n");
        return nullptr;
    }
    kprintf("[virtio-blk] pci %02x:%02x.%x vendor=%04x device=%04x\n",
            d.bus, d.dev, d.fn, d.vendor, d.device);

    pci_enable(&d);
    g_vblk.io = (uint16_t)pci_bar0_io(&d);

    /* Spec-mandated device init handshake. */
    cmd_status(&g_vblk, 0);                          /* reset */
    cmd_status(&g_vblk, VIRTIO_STATUS_ACK);
    cmd_status(&g_vblk, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Read host features, but ack none. That keeps us in pure legacy
       mode (no VERSION_1) and avoids any feature-specific config layout
       changes. */
    uint32_t host_feat = inl(g_vblk.io + VIRTIO_PCI_HOST_FEATURES);
    outl(g_vblk.io + VIRTIO_PCI_GUEST_FEATURES, 0);
    kprintf("[virtio-blk] host features=%08x, accepted=0\n", host_feat);

    virtq_setup(&g_vblk);

    /* INTx completion interrupt. Under the I/O APIC, apic_init already
       routed all PCI INTx GSIs to APIC_PCI_VECTOR (== IRQ 11 once
       dispatched), so we just register our handler there. On the legacy
       8259 we use the PIC IRQ the firmware wrote into PCI config 0x3C and
       unmask that line. Either way the line stays dormant until kmain
       does irq_enable. */
    g_vblk.irq_line = pci_read8(d.bus, d.dev, d.fn, PCI_INTERRUPT_LINE);
    g_vblk.wq.head = g_vblk.wq.tail = nullptr;
    g_vblk.completions = 0;
    if (apic_enabled()) {
        irq_register(APIC_PCI_VECTOR - 0x20, vblk_irq);
        g_vblk.irq_ok = 1;
        kprintf("[virtio-blk] INTx via I/O APIC -> vec %x\n", APIC_PCI_VECTOR);
    } else if (g_vblk.irq_line < NUM_IRQS) {
        irq_register(g_vblk.irq_line, vblk_irq);
        irq_unmask(g_vblk.irq_line);
        g_vblk.irq_ok = 1;
        kprintf("[virtio-blk] INTx on PIC IRQ %u\n", g_vblk.irq_line);
    } else {
        g_vblk.irq_ok = 0;
        kprintf("[virtio-blk] no usable IRQ line (%u); staying polled\n",
                g_vblk.irq_line);
    }

    /* Mark driver ok — device may start servicing now. */
    cmd_status(&g_vblk, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Device-specific config (no MSI-X): capacity is the first u64. */
    uint32_t cap_lo = inl(g_vblk.io + VIRTIO_PCI_CONFIG_NOMSI + 0);
    uint32_t cap_hi = inl(g_vblk.io + VIRTIO_PCI_CONFIG_NOMSI + 4);
    uint64_t capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    g_vblk.bd.dev.name   = "vblk0";
    g_vblk.bd.dev.cls    = DEV_BLOCK;
    g_vblk.bd.block_size = 512;
    g_vblk.bd.num_blocks = capacity;
    g_vblk.bd.read       = vblk_read;
    g_vblk.bd.write      = vblk_write;

    kprintf("[virtio-blk] queue size=%u, capacity=%lu sectors (%lu MiB)\n",
            g_vblk.qsize, (unsigned long)capacity,
            (unsigned long)(capacity / 2048));

    device_register(&g_vblk.bd.dev);
    return &g_vblk.bd;
}
