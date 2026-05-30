#include "kernel.h"
#include "io.h"
#include "pmm.h"
#include "paging.h"
#include "pci.h"
#include "virtio.h"
#include "irq.h"
#include "apic.h"
#include "task.h"
#include "net.h"

/*
 * Legacy virtio-net over PCI IO BAR0 (ROADMAP §5), same shape as virtio-blk.
 *
 * Two virtqueues: 0 = receive, 1 = transmit. Each buffer is prefixed by a
 * 10-byte virtio_net_hdr (we decline MRG_RXBUF / VERSION_1, so the header is the
 * legacy fixed size). RX buffers are pre-posted to the device; on the rx IRQ we
 * reap completed buffers, hand each frame to net_rx() (which queues it for the
 * net worker), and re-post the buffer. TX picks a free buffer from a small ring,
 * copies the frame in, and posts it; completed tx buffers are reaped lazily.
 */

#define RXQ 0
#define TXQ 1

#define NET_HDR_LEN  ((uint32_t)sizeof(virtio_net_hdr_t))   /* 10 */
#define BUF_SIZE     2048u                                   /* hdr + frame, padded */
#define N_RX         32
#define N_TX         32

typedef struct vq {
    uint16_t           qsize;
    virtq_desc_t      *desc;
    uint16_t          *avail;        /* &flags */
    uint16_t          *avail_idx;
    uint16_t          *avail_ring;
    uint16_t          *used;         /* &flags */
    uint16_t          *used_idx;
    virtq_used_elem_t *used_ring;
    uint16_t           last_used;
} vq_t;

typedef struct virtio_net {
    uint16_t io;
    vq_t     rx, tx;
    uint8_t  mac[ETH_ALEN];
    uint8_t *rxbuf[N_RX];            /* CPU pointers to the rx buffers */
    uint64_t rxbuf_pa[N_RX];
    uint8_t *txbuf[N_TX];
    uint64_t txbuf_pa[N_TX];
    uint16_t tx_next;                /* next tx buffer/descriptor to use */
    net_device_t dev;
} virtio_net_t;

static virtio_net_t g_net;

/* ---- low-level ---- */

static inline void cmd_status(uint8_t bits) { outb(g_net.io + VIRTIO_PCI_STATUS, bits); }

static void vq_alloc(uint16_t which, vq_t *q) {
    outw(g_net.io + VIRTIO_PCI_QUEUE_SEL, which);
    uint16_t n = inw(g_net.io + VIRTIO_PCI_QUEUE_NUM);
    if (n == 0) panic("virtio-net: queue %u absent", which);
    if (n > 256) n = 256;
    q->qsize = n;

    uint64_t desc_bytes  = (uint64_t)16 * n;
    uint64_t avail_bytes = 6 + 2 * (uint64_t)n;
    uint64_t used_off    = PAGE_ALIGN_UP(desc_bytes + avail_bytes);
    uint64_t used_bytes  = 6 + 8 * (uint64_t)n;
    uint64_t total       = PAGE_ALIGN_UP(used_off + used_bytes);

    uint64_t phys = pmm_alloc_pages(total / PAGE_SIZE);
    if (!phys) panic("virtio-net: out of memory for queue %u", which);
    uint8_t *base = (uint8_t *)phys_to_virt(phys);
    q->desc       = (virtq_desc_t *)base;
    q->avail      = (uint16_t *)(base + desc_bytes);
    q->avail_idx  = q->avail + 1;
    q->avail_ring = q->avail + 2;
    q->used       = (uint16_t *)(base + used_off);
    q->used_idx   = q->used + 1;
    q->used_ring  = (virtq_used_elem_t *)(base + used_off + 4);
    q->last_used  = 0;

    outl(g_net.io + VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys / PAGE_SIZE));
}

/* Make rx descriptor `i` available again (device writes hdr+frame into it). */
static void rx_post(uint16_t i) {
    vq_t *q = &g_net.rx;
    q->desc[i].addr  = g_net.rxbuf_pa[i];
    q->desc[i].len   = BUF_SIZE;
    q->desc[i].flags = VIRTQ_DESC_F_WRITE;
    q->desc[i].next  = 0;
    uint16_t idx = *q->avail_idx;
    q->avail_ring[idx % q->qsize] = i;
    __asm__ volatile("" ::: "memory");
    *q->avail_idx = idx + 1;
}

/* ---- transmit ---- */

static int net_tx(const void *frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) return -1;
    uint64_t f = irq_save();

    /* Reap completed tx buffers (we only need them marked free again). */
    g_net.tx.last_used = *g_net.tx.used_idx;

    uint16_t i = g_net.tx_next;
    g_net.tx_next = (uint16_t)((g_net.tx_next + 1) % N_TX);

    uint8_t *buf = g_net.txbuf[i];
    kmemset(buf, 0, NET_HDR_LEN);                  /* zero virtio_net_hdr */
    kmemcpy(buf + NET_HDR_LEN, frame, len);

    vq_t *q = &g_net.tx;
    q->desc[i].addr  = g_net.txbuf_pa[i];
    q->desc[i].len   = NET_HDR_LEN + len;
    q->desc[i].flags = 0;                          /* device reads */
    q->desc[i].next  = 0;
    uint16_t idx = *q->avail_idx;
    q->avail_ring[idx % q->qsize] = i;
    __asm__ volatile("" ::: "memory");
    *q->avail_idx = idx + 1;
    __asm__ volatile("" ::: "memory");
    outw(g_net.io + VIRTIO_PCI_QUEUE_NOTIFY, TXQ);

    irq_restore(f);
    return (int)len;
}

/* ---- receive (IRQ) ---- */

static void vnet_irq(uint8_t irq, regs_t *regs) {
    (void)irq; (void)regs;
    uint8_t isr = inb(g_net.io + VIRTIO_PCI_ISR);
    if (!(isr & 0x1)) return;                      /* not ours */

    vq_t *q = &g_net.rx;
    while (q->last_used != *q->used_idx) {
        virtq_used_elem_t *e = &q->used_ring[q->last_used % q->qsize];
        uint16_t i   = (uint16_t)e->id;
        uint32_t tot = e->len;
        q->last_used++;
        if (i < N_RX && tot > NET_HDR_LEN)
            net_rx(g_net.rxbuf[i] + NET_HDR_LEN, tot - NET_HDR_LEN);  /* queue for worker */
        if (i < N_RX) rx_post(i);                  /* hand the buffer back */
    }
    __asm__ volatile("" ::: "memory");
    outw(g_net.io + VIRTIO_PCI_QUEUE_NOTIFY, RXQ);
}

/* ---- init ---- */

static inline uint16_t bar0_io(pci_dev_t *d) {
    uint32_t bar = pci_read32(d->bus, d->dev, d->fn, PCI_BAR0);
    if ((bar & 1) == 0) panic("virtio-net: BAR0 is MMIO, expected IO");
    return (uint16_t)(bar & ~0x3u);
}

int virtio_net_init(void) {
    pci_dev_t d;
    if (!pci_find_subsys(VIRTIO_PCI_VENDOR, VIRTIO_SUBSYSTEM_NET, &d)) {
        kprintf("[virtio-net] no device found\n");
        return 0;
    }
    kprintf("[virtio-net] pci %02x:%02x.%x device=%04x\n", d.bus, d.dev, d.fn, d.device);

    uint16_t cmd = pci_read16(d.bus, d.dev, d.fn, PCI_COMMAND);
    pci_write16(d.bus, d.dev, d.fn, PCI_COMMAND, cmd | PCI_CMD_IO | PCI_CMD_BUS_MASTER);
    g_net.io = bar0_io(&d);

    cmd_status(0);                                 /* reset */
    cmd_status(VIRTIO_STATUS_ACK);
    cmd_status(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Accept only MAC: keeps the legacy 10-byte header and a stable config. */
    uint32_t host_feat = inl(g_net.io + VIRTIO_PCI_HOST_FEATURES);
    uint32_t want = host_feat & VIRTIO_NET_F_MAC;
    outl(g_net.io + VIRTIO_PCI_GUEST_FEATURES, want);

    /* Read the MAC from device config (offset 0x14, no MSI-X). */
    for (int i = 0; i < ETH_ALEN; i++)
        g_net.mac[i] = inb(g_net.io + VIRTIO_PCI_CONFIG_NOMSI + i);

    vq_alloc(RXQ, &g_net.rx);
    vq_alloc(TXQ, &g_net.tx);

    /* Allocate rx/tx buffers (one page each, plenty for hdr + 1514 frame) and
       pre-post all rx buffers so the device can fill them. */
    for (int i = 0; i < N_RX; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) panic("virtio-net: no memory for rx buffers");
        g_net.rxbuf_pa[i] = pa;
        g_net.rxbuf[i]    = (uint8_t *)phys_to_virt(pa);
        rx_post((uint16_t)i);
    }
    for (int i = 0; i < N_TX; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) panic("virtio-net: no memory for tx buffers");
        g_net.txbuf_pa[i] = pa;
        g_net.txbuf[i]    = (uint8_t *)phys_to_virt(pa);
    }
    g_net.tx_next = 0;

    /* Shared PCI INTx (chains with virtio-blk on APIC_PCI_VECTOR). */
    if (apic_enabled()) {
        irq_register(APIC_PCI_VECTOR - 0x20, vnet_irq);
        kprintf("[virtio-net] INTx via I/O APIC -> vec %x\n", APIC_PCI_VECTOR);
    } else {
        uint8_t line = pci_read8(d.bus, d.dev, d.fn, PCI_INTERRUPT_LINE);
        if (line < NUM_IRQS) { irq_register(line, vnet_irq); irq_unmask(line); }
    }

    cmd_status(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    outw(g_net.io + VIRTIO_PCI_QUEUE_NOTIFY, RXQ);   /* kick: rx buffers are ready */

    kprintf("[virtio-net] up: MAC %02x:%02x:%02x:%02x:%02x:%02x, rxq=%u txq=%u, feat=%08x\n",
            g_net.mac[0], g_net.mac[1], g_net.mac[2], g_net.mac[3], g_net.mac[4], g_net.mac[5],
            g_net.rx.qsize, g_net.tx.qsize, want);

    g_net.dev.name = "vnet0";
    g_net.dev.tx   = net_tx;
    kmemcpy(g_net.dev.mac, g_net.mac, ETH_ALEN);
    net_attach(&g_net.dev);
    return 1;
}
