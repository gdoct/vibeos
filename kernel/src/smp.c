#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "apic.h"
#include "timer.h"
#include "idt.h"
#include "smp.h"

/*
 * Application-processor bringup (ROADMAP §1, SMP stage A).
 *
 * For each CPU in the MADT other than the BSP: copy the trampoline to a low
 * page, point its parameter block at the bootstrap page tables (identity +
 * kernel + direct map), a fresh stack, and the C entry, then INIT-SIPI-SIPI.
 * The AP lands in ap_entry, switches to the master kernel tables, loads the
 * shared GDT/IDT, enables its LAPIC, marks itself online, and parks. (Running
 * tasks on APs is SMP stage B — the scheduler.)
 */

extern "C" uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern "C" uint8_t ap_tramp_cr3[], ap_tramp_stack[], ap_tramp_entry[];
extern "C" uint8_t boot_pml4[];          /* start.S bootstrap PML4 (phys == its VMA) */
extern "C" void gdt_init(void);

static struct cpu g_cpus[SMP_MAX_CPUS];
static volatile int g_online = 1;        /* BSP is online from the start */

int smp_cpu_count(void) { return g_online; }

/* Coarse millisecond delay off the 100 Hz tick (interrupts must be enabled). */
static void delay_ms(uint64_t ms) {
    uint64_t hz = timer_hz();
    if (!hz) return;
    uint64_t want = (ms * hz + 999) / 1000 + 1;
    uint64_t start = timer_ticks();
    while (timer_ticks() - start < want) __asm__ volatile("pause");
}

/* APs occupy slots 1..; each slot's apic_id is set before its SIPI, so a
   nonzero AP id matches exactly its slot. */
static int cpu_index_for_apic(uint32_t aid) {
    for (int i = 1; i < SMP_MAX_CPUS; i++)
        if (g_cpus[i].apic_id == aid) return i;
    return -1;
}

/* 64-bit entry for an AP, reached from the trampoline on a direct-map stack
   while still on the bootstrap page tables. */
extern "C" void ap_entry(void) {
    /* Move onto the real kernel tables (the direct-map stack stays mapped). */
    __asm__ volatile("mov %0, %%cr3" :: "r"(paging_kernel_pml4()) : "memory");

    gdt_init();              /* shared GDT + reload CS/data segments */
    idt_load();              /* shared IDT */
    apic_enable_local();     /* this CPU's LAPIC */

    uint32_t aid = apic_local_id();
    int ci = cpu_index_for_apic(aid);
    kprintf("[smp] CPU %d (apic %u) online\n", ci, aid);
    if (ci >= 0) __atomic_store_n(&g_cpus[ci].online, 1, __ATOMIC_RELEASE);

    for (;;) __asm__ volatile("sti; hlt");   /* park until SMP stage B */
}

void smp_init(void) {
    int n = apic_cpu_count();
    uint8_t bsp = apic_bsp_id();
    kprintf("[smp] MADT reports %d CPU(s); BSP apic=%u\n", n, bsp);
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;

    g_cpus[0].index = 0; g_cpus[0].apic_id = bsp; g_cpus[0].online = 1;
    if (n <= 1) return;

    /* Copy the trampoline blob to the low page. */
    uint64_t sz = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    uint8_t *tr = (uint8_t *)phys_to_virt(AP_TRAMPOLINE_PHYS);
    kmemcpy(tr, ap_trampoline_start, sz);

    uint64_t off_cr3 = (uintptr_t)ap_tramp_cr3   - (uintptr_t)ap_trampoline_start;
    uint64_t off_stk = (uintptr_t)ap_tramp_stack - (uintptr_t)ap_trampoline_start;
    uint64_t off_ent = (uintptr_t)ap_tramp_entry - (uintptr_t)ap_trampoline_start;

    uint64_t tcr3  = (uint64_t)(uintptr_t)boot_pml4;     /* phys of bootstrap PML4 */
    uint64_t entry = (uint64_t)(uintptr_t)ap_entry;

    int slot = 0;
    for (int i = 0; i < n; i++) {
        uint8_t aid = apic_cpu_apic_id(i);
        if (aid == bsp) continue;

        int ci = ++slot;                          /* 1..  (0 is the BSP) */
        g_cpus[ci].index = (uint32_t)ci;
        g_cpus[ci].apic_id = aid;
        g_cpus[ci].online = 0;

        /* Per-AP stack in the direct map (mapped under both bootstrap and
           master tables, and physically contiguous). */
        uint64_t sp_phys = pmm_alloc_pages(4);
        if (!sp_phys) panic("smp: no memory for AP stack");
        uint64_t sp_top = (uint64_t)phys_to_virt(sp_phys) + 4 * PAGE_SIZE;

        *(volatile uint64_t *)(tr + off_cr3) = tcr3;
        *(volatile uint64_t *)(tr + off_stk) = sp_top;
        *(volatile uint64_t *)(tr + off_ent) = entry;
        __asm__ volatile("" ::: "memory");

        apic_send_init(aid);
        delay_ms(10);
        apic_send_sipi(aid, AP_TRAMPOLINE_PHYS >> 12);
        delay_ms(2);
        apic_send_sipi(aid, AP_TRAMPOLINE_PHYS >> 12);

        uint64_t start = timer_ticks();
        while (!g_cpus[ci].online && timer_ticks() - start < 50)
            __asm__ volatile("pause");
        if (g_cpus[ci].online) g_online++;
        else kprintf("[smp] CPU apic=%u failed to start\n", aid);
    }

    kprintf("[smp] %d of %d CPU(s) online\n", g_online, n);
}
