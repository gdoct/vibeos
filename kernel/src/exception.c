#include "kernel.h"
#include "regs.h"
#include "task.h"
#include "smp.h"
#include "paging.h"
#include "signal.h"
#include "io.h"

/* ---- crash-safe diagnostics for a fault-while-handling-a-fault ------------
 * The normal handler below calls kprintf()/exc_name(), which read rodata. If
 * the fault being handled is itself caused by a bad rodata mapping (an SMP
 * page-table race we're chasing), those reads re-fault and the handler recurses
 * until the kernel stack overflows -> #DF -> silent triple-fault/reset.
 *
 * To make that debuggable we keep a per-CPU "currently handling a fault" record;
 * a nested entry dumps state using only raw port I/O and immediate chars (NO
 * rodata, NO kprintf), walks the page tables for the faulting address to expose
 * the offending PTE, and halts the CPU instead of recursing. */

struct exc_rec { int active; uint64_t vec, err, rip, cr2; int tid; };
static struct exc_rec g_exc[SMP_MAX_CPUS];

static void rs_putc(char c) {
    while (!(inb(0x3FD) & 0x20)) { }     /* COM1 LSR bit5: THR empty */
    if (c == '\n') { outb(0x3F8, '\r'); while (!(inb(0x3FD) & 0x20)) { } }
    outb(0x3F8, (uint8_t)c);
}
static void rs_hex(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4) {
        unsigned n = (unsigned)((v >> i) & 0xf);
        rs_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
    }
}
/* Print " <c>=<hex>" with no rodata (label is an immediate char). */
static void rs_kv(char c, uint64_t v) { rs_putc(' '); rs_putc(c); rs_putc('='); rs_hex(v); }

/* Walk the active page tables for `va` and dump each level's entry on its own
   line. A reserved-bit (#PF err bit3) fault means one of these has a bit set
   that the CPU rejects — this shows which level and the raw value. */
static void rs_pagewalk(uint64_t va) {
    const uint64_t PA = 0x000FFFFFFFFFF000ULL;     /* PTE phys-address field */
    uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t tbl = cr3 & PA;
    int shift[4] = { 39, 30, 21, 12 };
    for (int lvl = 0; lvl < 4; lvl++) {
        uint64_t *t = (uint64_t *)phys_to_virt(tbl);
        uint64_t idx = (va >> shift[lvl]) & 0x1ff;
        uint64_t e = t[idx];
        rs_putc('\n'); rs_putc('L'); rs_putc((char)('4' - lvl));   /* L4..L1 */
        rs_kv('i', idx); rs_kv('e', e);
        if (!(e & 1)) break;                       /* not present: stop */
        if (lvl < 3 && (e & 0x80)) break;          /* huge page: leaf, stop */
        tbl = e & PA;
    }
    rs_putc('\n');
}

static void exc_nested_dump(int cpu, regs_t *r, uint64_t cr2) {
    struct exc_rec *o = &g_exc[cpu];
    /* "!!NEST" then original fault (lowercase) and this nested fault (uppercase) */
    rs_putc('\n'); rs_putc('!'); rs_putc('!');
    rs_putc('N'); rs_putc('E'); rs_putc('S'); rs_putc('T');
    rs_kv('C', (uint64_t)cpu);
    rs_putc(' '); rs_putc('o'); rs_putc('r'); rs_putc('i'); rs_putc('g');
    rs_kv('v', o->vec); rs_kv('e', o->err); rs_kv('r', o->rip); rs_kv('2', o->cr2); rs_kv('t', (uint64_t)o->tid);
    rs_putc(' '); rs_putc('n'); rs_putc('e'); rs_putc('s'); rs_putc('t');
    rs_kv('V', r->vector); rs_kv('E', r->error_code); rs_kv('R', r->rip); rs_kv('2', cr2);
    rs_pagewalk(cr2);                              /* show the offending PTE chain */
    for (;;) __asm__ volatile("cli; hlt");
}

static const char *exc_name(uint64_t v) {
    switch (v) {
    case 0:  return "#DE Divide-by-zero";
    case 1:  return "#DB Debug";
    case 2:  return "NMI";
    case 3:  return "#BP Breakpoint";
    case 4:  return "#OF Overflow";
    case 5:  return "#BR Bound Range";
    case 6:  return "#UD Invalid Opcode";
    case 7:  return "#NM Device Not Available";
    case 8:  return "#DF Double Fault";
    case 10: return "#TS Invalid TSS";
    case 11: return "#NP Segment Not Present";
    case 12: return "#SS Stack-Segment Fault";
    case 13: return "#GP General Protection";
    case 14: return "#PF Page Fault";
    case 16: return "#MF x87 FP";
    case 17: return "#AC Alignment Check";
    case 18: return "#MC Machine Check";
    case 19: return "#XM SIMD FP";
    case 20: return "#VE Virtualization";
    case 21: return "#CP Control Protection";
    default: return "(unknown)";
    }
}

extern "C"
void exception_handler(regs_t *r) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    int cpu = smp_cpu_index();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) cpu = 0;
    if (g_exc[cpu].active) {                 /* fault while handling a fault */
        exc_nested_dump(cpu, r, cr2);        /* raw dump + page walk, then halt (no return) */
    }
    g_exc[cpu].active = 1;
    g_exc[cpu].vec = r->vector; g_exc[cpu].err = r->error_code;
    g_exc[cpu].rip = r->rip;    g_exc[cpu].cr2 = cr2;
    g_exc[cpu].tid = task_current() ? task_current()->id : -1;

    /* Copy-on-write repair: a ring-3 write to a present, user, read-only page
       that carries PTE_COW is fixed up here (private copy, or re-grant write if
       sole owner). On success we return and the faulting store is retried by the
       iretq in isr_common. (#PF error: bit1 = write, bit2 = user.) */
    if (r->vector == 14 && (r->error_code & 2) && (r->error_code & 4)) {
        task_t *t = task_current();
        if (t && t->vm && paging_cow_fault(t->vm, cr2)) {
            g_exc[cpu].active = 0;
            return;
        }
    }

    /* A fault from ring 3 is the process's bug, not the kernel's. Turn it into
       the matching signal (ROADMAP §3): a handler (if installed) runs and the
       faulting instruction is retried; otherwise the default action terminates
       the process. Either way the kernel keeps running. */
    if ((r->cs & 3) == 3 && sched_active()) {
        int sig;
        switch (r->vector) {
        case 6:  sig = SIGILL;  break;          /* #UD invalid opcode */
        case 0:  sig = SIGFPE;  break;          /* #DE divide error  */
        case 16: case 19: sig = SIGFPE; break;  /* x87 / SIMD FP     */
        case 17: sig = SIGBUS;  break;          /* #AC alignment     */
        default: sig = SIGSEGV; break;          /* #PF/#GP/#SS/...   */
        }
        task_t *t = task_current();
        kprintf("\n[fault] %s in user task %d \"%s\": err=%lx rip=%016lx cr2=%016lx -> signal %d\n",
                exc_name(r->vector), t ? t->id : -1, t ? t->name : "?",
                r->error_code, r->rip, cr2, sig);
        g_exc[cpu].active = 0;
        signals_raise_fault(r, sig);    /* enters handler (returns) or terminates */
        return;
    }

    kprintf("\n!! EXCEPTION %lu (%s) err=%lx\n",
            r->vector, exc_name(r->vector), r->error_code);

    if (r->vector == 14) {
        /* #PF error code bits: P present, W/R write, U/S user, RSVD, I/D. */
        uint64_t e = r->error_code;
        kprintf("   page fault @ %016lx: %s, %s, %s%s\n", cr2,
                (e & 1) ? "protection-violation" : "not-present",
                (e & 2) ? "write" : "read",
                (e & 4) ? "user" : "kernel",
                (e & 16) ? ", instr-fetch" : "");
    }
    kprintf("   rip=%016lx cs=%lx rflags=%016lx\n", r->rip, r->cs, r->rflags);
    kprintf("   rsp=%016lx ss=%lx cr2=%016lx\n",    r->rsp, r->ss, cr2);
    kprintf("   rax=%016lx rbx=%016lx rcx=%016lx\n", r->rax, r->rbx, r->rcx);
    kprintf("   rdx=%016lx rsi=%016lx rdi=%016lx\n", r->rdx, r->rsi, r->rdi);
    kprintf("   rbp=%016lx  r8=%016lx  r9=%016lx\n", r->rbp, r->r8,  r->r9);
    kprintf("   r10=%016lx r11=%016lx r12=%016lx\n", r->r10, r->r11, r->r12);
    kprintf("   r13=%016lx r14=%016lx r15=%016lx\n", r->r13, r->r14, r->r15);
    { task_t *ct = task_current();
      kprintf("   cpu=%d cur=%d \"%s\" vm=%p\n", smp_cpu_index(),
              ct ? ct->id : -1, ct ? ct->name : "?", ct ? (void*)ct->vm : (void*)0); }

    panic("unhandled exception");
}
