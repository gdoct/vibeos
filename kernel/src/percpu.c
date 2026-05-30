#include "kernel.h"
#include "idt.h"
#include "smp.h"
#include "percpu.h"

/*
 * Per-CPU control blocks, GS base, and per-CPU TSS (ROADMAP §2).
 *
 * In long mode the TSS only supplies rsp0..rsp2 + the IST stacks; we use rsp0 so
 * a ring-3 -> ring-0 trap lands on the running task's kernel stack. Each CPU has
 * its own TSS so user tasks can run on every core (not just the BSP): CPU i's
 * descriptor sits at GDT selector 0x28 + i*0x10 and each core ltr's its own.
 *
 * The CPU's GS base points at its percpu block; the SYSCALL stub swapgs's to it
 * to find the kernel stack without touching a user register (see usermode.S).
 */

#define MSR_GS_BASE         0xC0000101u
#define MSR_KERNEL_GS_BASE  0xC0000102u

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)));
}

static struct percpu g_percpu[SMP_MAX_CPUS];

/* The TSS-descriptor area of the shared GDT (gdt.S): 2 quads per CPU. */
extern "C" uint64_t gdt_tss[];

static_assert(__builtin_offsetof(struct percpu, user_scratch) == PCPU_USER_SCRATCH,
              "PCPU_USER_SCRATCH offset mismatch");
static_assert(__builtin_offsetof(struct percpu, kernel_rsp) == PCPU_KERNEL_RSP,
              "PCPU_KERNEL_RSP offset mismatch");

/* Write CPU `idx`'s 16-byte 64-bit-TSS descriptor (type 0x9, present). */
static void set_tss_desc(int idx, const tss_t *tss) {
    uint64_t base  = (uint64_t)(uintptr_t)tss;
    uint32_t limit = sizeof(tss_t) - 1;
    uint64_t lo = 0;
    lo |= (uint64_t)(limit & 0xFFFF);
    lo |= (uint64_t)(base & 0xFFFFFF) << 16;
    lo |= (uint64_t)0x89 << 40;                  /* P=1, DPL=0, type=9 */
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;
    lo |= (uint64_t)((base >> 24) & 0xFF) << 56;
    gdt_tss[idx * 2]     = lo;
    gdt_tss[idx * 2 + 1] = (base >> 32) & 0xFFFFFFFF;
}

/* Enable SSE on the calling CPU: clear CR0.EM (no x87 emulation), set CR0.MP,
   and set CR4.OSFXSR | CR4.OSXMMEXCPT so SSE instructions and FXSAVE work. The
   BSP inherits this from UEFI firmware, but APs come up through the real-mode
   trampoline with it off — without this a static musl binary's SSE memset/memcpy
   #UDs the moment it runs on an AP (ROADMAP §2). */
static void enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(1ULL << 2)) | (1ULL << 1);          /* EM=0, MP=1 */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);                  /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
}

void percpu_init(int idx) {
    struct percpu *pc = &g_percpu[idx];
    kmemset(pc, 0, sizeof *pc);
    pc->index = (uint32_t)idx;
    pc->tss.iomap_base = (uint16_t)sizeof(tss_t);  /* no I/O bitmap */

    enable_sse();

    set_tss_desc(idx, &pc->tss);
    uint16_t sel = (uint16_t)(TSS_SEL + idx * 0x10);
    __asm__ volatile("ltr %w0" :: "r"(sel));

    /* Both GS bases start at the percpu block. While in the kernel GS base must
       be this block (every entry swapgs restores it); enter_user/sysret swapgs
       keep KERNEL_GS_BASE pointing here for the next entry. */
    wrmsr(MSR_GS_BASE,        (uint64_t)(uintptr_t)pc);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)pc);

    kprintf("[percpu] CPU %d: TSS sel=%x, GS base=%p\n", idx, sel, (void *)pc);
}

struct percpu *percpu_current(void) { return &g_percpu[smp_cpu_index()]; }

void percpu_set_kernel_stack(uint64_t top) {
    struct percpu *pc = percpu_current();
    pc->kernel_rsp = top;
    pc->tss.rsp0   = top;
}
