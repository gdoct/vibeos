#include "kernel.h"
#include "io.h"
#include "pmm.h"
#include "paging.h"
#include "pci.h"
#include "virtio.h"
#include "csprng.h"

/*
 * Legacy virtio-rng (virtio entropy device, ROADMAP §2), same shape as
 * virtio-blk/net but minimal: one virtqueue. We use it only to seed the CSPRNG
 * at boot, so the read is *polled* — no IRQ wiring. Place a write buffer on the
 * single queue, notify, spin on the used ring, then fold the bytes into the
 * DRBG via csprng_add_entropy().
 *
 * Present only when QEMU is launched with -device virtio-rng-pci; absent is fine
 * (RDRAND + RDTSC jitter still seed the DRBG). virtio_rng_seed() is best-effort.
 */

#define RNG_BYTES 64

static inline uint16_t bar0_io(pci_dev_t *d) {
    uint32_t bar = pci_read32(d->bus, d->dev, d->fn, PCI_BAR0);
    return (uint16_t)(bar & ~0x3u);
}

extern "C" int virtio_rng_seed(void) {
    pci_dev_t d;
    if (!pci_find_subsys(VIRTIO_PCI_VENDOR, VIRTIO_SUBSYSTEM_ENTROPY, &d))
        return 0;

    uint16_t cmd = pci_read16(d.bus, d.dev, d.fn, PCI_COMMAND);
    pci_write16(d.bus, d.dev, d.fn, PCI_COMMAND, cmd | PCI_CMD_IO | PCI_CMD_BUS_MASTER);
    uint16_t io = bar0_io(&d);

    /* Reset + ACK + DRIVER. */
    outb(io + VIRTIO_PCI_STATUS, 0);
    outb(io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
    outb(io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    outl(io + VIRTIO_PCI_GUEST_FEATURES, 0);   /* no features needed */

    /* Set up queue 0. */
    outw(io + VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t n = inw(io + VIRTIO_PCI_QUEUE_NUM);
    if (n == 0) return 0;
    if (n > 256) n = 256;

    uint64_t desc_bytes  = (uint64_t)16 * n;
    uint64_t avail_bytes = 6 + 2 * (uint64_t)n;
    uint64_t used_off    = PAGE_ALIGN_UP(desc_bytes + avail_bytes);
    uint64_t used_bytes  = 6 + 8 * (uint64_t)n;
    uint64_t total       = PAGE_ALIGN_UP(used_off + used_bytes);

    uint64_t qphys = pmm_alloc_pages(total / PAGE_SIZE);
    if (!qphys) return 0;
    uint8_t *base = (uint8_t *)phys_to_virt(qphys);
    for (uint64_t i = 0; i < total; i++) base[i] = 0;
    virtq_desc_t      *desc       = (virtq_desc_t *)base;
    uint16_t          *avail_idx  = (uint16_t *)(base + desc_bytes) + 1;
    uint16_t          *avail_ring = (uint16_t *)(base + desc_bytes) + 2;
    uint16_t          *used_idx   = (uint16_t *)(base + used_off) + 1;

    outl(io + VIRTIO_PCI_QUEUE_PFN, (uint32_t)(qphys / PAGE_SIZE));
    outb(io + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* A single device-writable buffer for the entropy bytes. */
    uint64_t bphys = pmm_alloc_page();
    if (!bphys) return 0;
    uint8_t *buf = (uint8_t *)phys_to_virt(bphys);
    for (int i = 0; i < RNG_BYTES; i++) buf[i] = 0;

    desc[0].addr  = bphys;
    desc[0].len   = RNG_BYTES;
    desc[0].flags = VIRTQ_DESC_F_WRITE;
    desc[0].next  = 0;
    avail_ring[0] = 0;
    __asm__ volatile("" ::: "memory");
    *avail_idx = 1;
    __asm__ volatile("" ::: "memory");
    outw(io + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Poll the used ring (bounded spin — the device fills it promptly). */
    for (int spin = 0; spin < 10000000; spin++) {
        __asm__ volatile("" ::: "memory");
        if (*used_idx != 0) {
            (void)inb(io + VIRTIO_PCI_ISR);     /* ack */
            csprng_add_entropy(buf, RNG_BYTES);
            kprintf("[virtio-rng] seeded CSPRNG with %d bytes of hardware entropy\n",
                    RNG_BYTES);
            return 1;
        }
    }
    return 0;
}
