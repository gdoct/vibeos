#include "kernel.h"
#include "regs.h"
#include "task.h"
#include "smp.h"
#include "paging.h"
#include "signal.h"

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

    /* Copy-on-write repair: a ring-3 write to a present, user, read-only page
       that carries PTE_COW is fixed up here (private copy, or re-grant write if
       sole owner). On success we return and the faulting store is retried by the
       iretq in isr_common. (#PF error: bit1 = write, bit2 = user.) */
    if (r->vector == 14 && (r->error_code & 2) && (r->error_code & 4)) {
        task_t *t = task_current();
        if (t && t->vm && paging_cow_fault(t->vm, cr2))
            return;
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
