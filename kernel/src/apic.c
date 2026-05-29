#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "idt.h"
#include "paging.h"
#include "apic.h"

/*
 * Local APIC + I/O APIC bring-up.
 *
 * We parse the ACPI MADT to find the LAPIC and I/O APIC, enable the
 * LAPIC, and move the two interrupts we actually use onto it:
 *   - the system tick, via the LAPIC's own timer (calibrated against the
 *     PIT), delivered on APIC_TIMER_VECTOR;
 *   - PCI INTx (virtio-blk), via the I/O APIC. Rather than parse the ACPI
 *     _PRT to learn which GSI a device's pin lands on, we route *all* PCI
 *     INTx GSIs (16..23 on q35) to one shared vector and let the device's
 *     ISR register say whether the interrupt was its — which is exactly
 *     how shared, level-triggered PCI interrupts are meant to work.
 *
 * MMIO is reached through the direct map (phys_to_virt); the LAPIC at
 * 0xFEE00000 and I/O APIC at 0xFEC00000 are well within it.
 */

/* ---- ACPI table shapes ---- */

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_t;

typedef struct {
    acpi_sdt_t hdr;
    uint32_t   local_apic_addr;
    uint32_t   flags;
    uint8_t    entries[];
} __attribute__((packed)) acpi_madt_t;

/* MADT entry types we care about. */
#define MADT_IOAPIC          1
#define MADT_INT_OVERRIDE    2
#define MADT_LAPIC_OVERRIDE  5

/* ---- LAPIC registers (byte offsets from base) ---- */
#define LAPIC_ID         0x020
#define LAPIC_EOI        0x0B0
#define LAPIC_SVR        0x0F0
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_LVT_LINT0  0x350
#define LAPIC_LVT_LINT1  0x360
#define LAPIC_LVT_ERROR  0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0
#define LAPIC_ICR_LO     0x300   /* interrupt command (write low triggers) */
#define LAPIC_ICR_HI     0x310   /* destination APIC id in bits 31:24 */
#define ICR_DELIVERY_PENDING (1u << 12)

#define MADT_LAPIC       0       /* Processor Local APIC entry */
#define MAX_CPUS         8

#define LVT_MASKED       (1u << 16)
#define LVT_PERIODIC     (1u << 17)
#define SVR_ENABLE       (1u << 8)
#define TIMER_DIV_16     0x3

#define IA32_APIC_BASE   0x1B
#define APIC_BASE_ENABLE (1u << 11)

static uint64_t g_lapic_base = 0;
static uint64_t g_ioapic_base = 0;
static uint32_t g_ioapic_gsi_base = 0;
static uint8_t  g_bsp_id = 0;
static int      g_enabled = 0;
static uint8_t  g_cpu_ids[MAX_CPUS];   /* APIC ids of enabled CPUs (from MADT) */
static int      g_ncpu = 0;
static uint32_t g_timer_init = 0;      /* calibrated LAPIC-timer initial count */

int apic_enabled(void) { return g_enabled; }

/* ---- MSR + MMIO helpers ---- */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline volatile uint32_t *lapic_reg(uint32_t off) {
    return (volatile uint32_t *)((uint8_t *)phys_to_virt(g_lapic_base) + off);
}
static uint32_t lapic_read(uint32_t off)        { return *lapic_reg(off); }
static void     lapic_write(uint32_t off, uint32_t v) { *lapic_reg(off) = v; }

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

static uint32_t ioapic_read(uint32_t reg) {
    volatile uint32_t *base = (volatile uint32_t *)phys_to_virt(g_ioapic_base);
    base[0] = reg;                 /* IOREGSEL */
    return base[4];                /* IOWIN (offset 0x10 = index 4) */
}
static void ioapic_write(uint32_t reg, uint32_t v) {
    volatile uint32_t *base = (volatile uint32_t *)phys_to_virt(g_ioapic_base);
    base[0] = reg;
    base[4] = v;
}

/* Program one redirection entry. low_active/level select PCI-style
   (active-low, level) vs ISA-style (active-high, edge). */
static void ioapic_route(uint32_t gsi, uint8_t vector, uint8_t dest,
                         int level, int low_active, int masked) {
    uint32_t idx = gsi - g_ioapic_gsi_base;
    uint32_t lo = vector;                       /* fixed delivery, phys dest */
    if (low_active) lo |= (1u << 13);
    if (level)      lo |= (1u << 15);
    if (masked)     lo |= (1u << 16);
    uint32_t hi = (uint32_t)dest << 24;
    ioapic_write(0x10 + 2 * idx + 1, hi);
    ioapic_write(0x10 + 2 * idx,     lo);
}

/* ---- ACPI table walk ---- */

static int checksum_ok(const void *p, uint32_t len) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

static const acpi_madt_t *find_madt(const acpi_rsdp_t *rsdp) {
    /* Prefer the XSDT (64-bit pointers) when present. */
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        const acpi_sdt_t *xsdt = (const acpi_sdt_t *)phys_to_virt(rsdp->xsdt_addr);
        uint32_t n = (xsdt->length - sizeof(acpi_sdt_t)) / 8;
        const uint64_t *ptrs = (const uint64_t *)(xsdt + 1);
        for (uint32_t i = 0; i < n; i++) {
            const acpi_sdt_t *t = (const acpi_sdt_t *)phys_to_virt(ptrs[i]);
            if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
                t->signature[2] == 'I' && t->signature[3] == 'C')
                return (const acpi_madt_t *)t;
        }
        return nullptr;
    }
    const acpi_sdt_t *rsdt = (const acpi_sdt_t *)phys_to_virt(rsdp->rsdt_addr);
    uint32_t n = (rsdt->length - sizeof(acpi_sdt_t)) / 4;
    const uint32_t *ptrs = (const uint32_t *)(rsdt + 1);
    for (uint32_t i = 0; i < n; i++) {
        const acpi_sdt_t *t = (const acpi_sdt_t *)phys_to_virt(ptrs[i]);
        if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
            t->signature[2] == 'I' && t->signature[3] == 'C')
            return (const acpi_madt_t *)t;
    }
    return nullptr;
}

/* Pull LAPIC + first I/O APIC out of the MADT. Returns 0 on success. */
static int parse_madt(const acpi_madt_t *madt) {
    g_lapic_base = madt->local_apic_addr;

    const uint8_t *p   = madt->entries;
    const uint8_t *end = (const uint8_t *)madt + madt->hdr.length;
    int found_ioapic = 0;
    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2 || p + len > end) break;
        switch (type) {
        case MADT_LAPIC:                            /* type 0: a CPU */
            if ((*(const uint32_t *)(p + 4) & 1) && g_ncpu < MAX_CPUS)
                g_cpu_ids[g_ncpu++] = p[3];         /* p[3] = APIC id */
            break;
        case MADT_IOAPIC:
            if (!found_ioapic) {
                g_ioapic_base     = *(const uint32_t *)(p + 4);
                g_ioapic_gsi_base = *(const uint32_t *)(p + 8);
                found_ioapic = 1;
            }
            break;
        case MADT_LAPIC_OVERRIDE:
            g_lapic_base = *(const uint64_t *)(p + 4);
            break;
        default:
            break;
        }
        p += len;
    }
    return found_ioapic ? 0 : -1;
}

/* ---- LAPIC timer calibration via PIT channel 2 ---- */

extern "C" void isr_spurious(void);

static uint64_t calibrate_timer(void) {
    /* Channel 2: speaker off (bit1=0), gate enabled (bit0=1). */
    outb(0x61, (uint8_t)((inb(0x61) & ~0x02) | 0x01));
    /* Mode 1 (hardware re-triggerable one-shot), lo/hi byte, channel 2. */
    outb(0x43, 0xB2);
    uint16_t count = 11931;            /* ~10 ms at 1.193182 MHz */
    outb(0x42, (uint8_t)(count & 0xFF));
    inb(0x60);                          /* brief settle */
    outb(0x42, (uint8_t)(count >> 8));

    /* Re-trigger the count by toggling the gate low->high. */
    uint8_t g = inb(0x61) & ~0x01;
    outb(0x61, g);
    outb(0x61, (uint8_t)(g | 0x01));

    /* Free-run the LAPIC timer from max while the PIT counts down. */
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    while (!(inb(0x61) & 0x20)) { }     /* wait for PIT OUT2 high */

    uint32_t remaining = lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);   /* stop */

    uint32_t elapsed = 0xFFFFFFFFu - remaining;   /* LAPIC ticks in ~10 ms */
    return (uint64_t)elapsed * 100;               /* -> ticks per second */
}

/* ---- public init ---- */

int apic_init(const BootInfo *bi, uint32_t tick_hz) {
    if (!bi->rsdp) {
        kprintf("[apic] no RSDP in BootInfo; staying on PIC\n");
        return 0;
    }
    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)phys_to_virt(bi->rsdp);
    if (!checksum_ok(rsdp, 20)) {       /* ACPI 1.0 portion */
        kprintf("[apic] RSDP checksum bad; staying on PIC\n");
        return 0;
    }
    const acpi_madt_t *madt = find_madt(rsdp);
    if (!madt || parse_madt(madt) != 0) {
        kprintf("[apic] no usable MADT/IOAPIC; staying on PIC\n");
        return 0;
    }
    kprintf("[apic] MADT: lapic=%lx ioapic=%lx gsi_base=%u\n",
            (unsigned long)g_lapic_base, (unsigned long)g_ioapic_base,
            g_ioapic_gsi_base);

    /* Global-enable the LAPIC via its base MSR, then software-enable it
       and point the spurious vector at a do-nothing stub. */
    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | APIC_BASE_ENABLE);
    idt_set_vector(APIC_SPURIOUS_VECTOR, (void *)isr_spurious);
    lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    g_bsp_id = (uint8_t)(lapic_read(LAPIC_ID) >> 24);

    /* Park the local interrupt lines we don't use. */
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);

    /* Mask every I/O APIC redirection entry, then route the PCI INTx GSIs
       (16..23 on q35) to the shared PCI vector, level/active-low. */
    uint32_t max_redir = (ioapic_read(1) >> 16) & 0xFF;
    for (uint32_t gsi = g_ioapic_gsi_base; gsi <= g_ioapic_gsi_base + max_redir; gsi++)
        ioapic_route(gsi, 0, 0, 0, 0, /*masked=*/1);
    for (uint32_t gsi = 16; gsi <= 23 && gsi <= g_ioapic_gsi_base + max_redir; gsi++)
        ioapic_route(gsi, APIC_PCI_VECTOR, g_bsp_id, /*level=*/1, /*low_active=*/1, 0);

    /* Calibrate, then start the LAPIC timer as the periodic system tick.
       Save the count so each AP can program its own timer at the same rate. */
    uint64_t per_sec = calibrate_timer();
    uint32_t init_count = (uint32_t)(per_sec / (tick_hz ? tick_hz : 100));
    if (init_count == 0) init_count = 1;
    g_timer_init = init_count;
    apic_start_local_timer();

    g_enabled = 1;
    irq_set_apic_mode(1);
    kprintf("[apic] LAPIC id=%u, timer %lu ticks/s (init=%u), PCI INTx -> vec %x\n",
            g_bsp_id, (unsigned long)per_sec, init_count, APIC_PCI_VECTOR);
    return 1;
}

/* ---- SMP support (ROADMAP §1) ---- */

int     apic_cpu_count(void)       { return g_ncpu; }
uint8_t apic_cpu_apic_id(int i)    { return (i >= 0 && i < g_ncpu) ? g_cpu_ids[i] : 0; }
uint8_t apic_bsp_id(void)          { return g_bsp_id; }
uint32_t apic_local_id(void)       { return lapic_read(LAPIC_ID) >> 24; }

static void icr_send(uint8_t dest, uint32_t low) {
    lapic_write(LAPIC_ICR_HI, (uint32_t)dest << 24);
    lapic_write(LAPIC_ICR_LO, low);
    while (lapic_read(LAPIC_ICR_LO) & ICR_DELIVERY_PENDING) __asm__ volatile("pause");
}

/* INIT IPI (assert, edge) then a STARTUP IPI carrying the trampoline page as
   its vector. The BSP paces these with delays (see smp.c). */
void apic_send_init(uint8_t dest)              { icr_send(dest, 0x00004500u); }
void apic_send_sipi(uint8_t dest, uint8_t vec) { icr_send(dest, 0x00004600u | vec); }

/* Software-enable the calling CPU's local APIC (an AP does this for itself;
   the BSP already did it in apic_init). */
void apic_enable_local(void) {
    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | APIC_BASE_ENABLE);
    lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
}

/* Start this CPU's LAPIC timer at the calibrated rate (periodic, the system
   tick vector). The BSP calls it from apic_init; each AP calls it once up. */
void apic_start_local_timer(void) {
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_VECTOR | LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, g_timer_init ? g_timer_init : 1);
}
