#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "apic.h"
#include "timer.h"
#include "idt.h"
#include "task.h"
#include "percpu.h"
#include "usermode.h"
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

extern "C" void tlb_shootdown_isr(void);

static struct cpu g_cpus[SMP_MAX_CPUS];
static volatile int g_online = 1;        /* BSP is online from the start */

int smp_cpu_count(void) { return g_online; }

/* ---- TLB shootdown (ROADMAP §2) ----
 *
 * Reload-CR3-on-context-switch already flushes a migrating (single-threaded)
 * address space's stale entries, and we use no global pages or PCIDs, so the
 * unmap hot paths don't currently need a shootdown. This is the cross-CPU
 * primitive for when they will (shared/threaded address spaces) — and the same
 * IPI machinery that §3 uses to poke a task running on another core. The sender
 * must have interrupts enabled while it waits for acks, so callers invoke it
 * from ordinary kernel context, never from inside a syscall (IF=0) or under
 * sched_lock. */
static volatile int g_tlb_acks = 0;

static inline void flush_local_tlb(void) {
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

extern "C" void tlb_shootdown_handler(void) {
    flush_local_tlb();
    __atomic_fetch_add(&g_tlb_acks, 1, __ATOMIC_ACQ_REL);
    lapic_eoi();
}

void tlb_shootdown_all(void) {
    flush_local_tlb();                       /* always flush the caller's own TLB */
    int others = g_online - 1;
    if (others <= 0 || !apic_enabled()) return;
    __atomic_store_n(&g_tlb_acks, 0, __ATOMIC_RELEASE);
    apic_send_ipi_others(APIC_TLB_VECTOR);
    while (__atomic_load_n(&g_tlb_acks, __ATOMIC_ACQUIRE) < others)
        __asm__ volatile("pause");
}

/* Serialize concurrent shootdown senders. g_tlb_acks is a single shared counter,
   so only one broadcast may be in flight at a time. The lock is taken with
   interrupts ENABLED (see tlb_shootdown_user) so a sender waiting for it still
   services another core's shootdown IPI — otherwise two cores shooting each other
   would deadlock. */
static volatile uint32_t g_sd_lock = 0;

/* Cross-core TLB flush from a syscall path (ROADMAP §"User tasks on all cores").
   Syscalls run with IF cleared (SFMASK), but the ack-wait needs interrupts on, so
   enable them around the shootdown and restore the prior IF afterwards. Only the
   memory syscalls that demote/remove mappings on a *shared* (multithreaded)
   address space call this; single-threaded processes never need it. */
void tlb_shootdown_user(void) {
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0" : "=r"(fl));   /* save caller IF */
    __asm__ volatile("sti");
    while (__atomic_exchange_n(&g_sd_lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    tlb_shootdown_all();
    __atomic_store_n(&g_sd_lock, 0u, __ATOMIC_RELEASE);
    if (!(fl & (1ull << 9))) __asm__ volatile("cli");   /* restore IF=off if it was */
}

/* Boot-time check that the IPI path works: every other online CPU should ack a
   broadcast shootdown. Runs on the BSP with interrupts enabled (after smp_init,
   before the scheduler loop), where the APs are idling in scheduler(). */
void smp_ipi_selftest(void) {
    if (g_online <= 1) return;
    tlb_shootdown_all();
    kprintf("[smp] TLB-shootdown IPI: %d CPU(s) acked\n", g_online - 1);
}

/* Index of the calling CPU (0 = BSP). Before the LAPIC is up only the BSP
   runs, so return 0; afterwards map the local APIC id to its slot. */
int smp_cpu_index(void) {
    if (!apic_enabled()) return 0;
    uint32_t aid = apic_local_id();
    for (int i = 0; i < SMP_MAX_CPUS; i++)
        if (g_cpus[i].apic_id == aid && (i == 0 || g_cpus[i].index == (uint32_t)i))
            return i;
    return 0;   /* not yet registered (early): treat as BSP */
}

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
    percpu_init(ci >= 0 ? ci : 0);  /* this CPU's TSS + GS base (after gdt_init) */
    syscall_init();              /* EFER.SCE + STAR/LSTAR/SFMASK on this CPU, so
                                    user tasks can `syscall` here too (ROADMAP §2) */

    apic_start_local_timer();    /* this CPU's preemption tick */

    kprintf("[smp] CPU %d (apic %u) online\n", ci, aid);
    if (ci >= 0) __atomic_store_n(&g_cpus[ci].online, 1, __ATOMIC_RELEASE);

    scheduler();                 /* join the scheduler; never returns */
}

void smp_init(void) {
    int n = apic_cpu_count();
    uint8_t bsp = apic_bsp_id();
    kprintf("[smp] MADT reports %d CPU(s); BSP apic=%u\n", n, bsp);
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;

    /* Wire the TLB-shootdown IPI into the shared IDT before the APs join (they
       idt_load the same table). */
    idt_set_vector(APIC_TLB_VECTOR, (void *)tlb_shootdown_isr);

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
