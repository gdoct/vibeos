#include "kernel.h"
#include "regs.h"
#include "task.h"
#include "paging.h"

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

    /* A fault that came from ring 3 (or otherwise belongs to a running user
       task) is the process's bug, not the kernel's — kill it and reschedule
       rather than taking the whole system down (ROADMAP §1.1: guarded user
       stacks / bad user pointers must not panic). */
    if ((r->cs & 3) == 3 && sched_active()) {
        task_t *t = task_current();
        kprintf("\n[fault] %s in user task %d \"%s\": err=%lx rip=%016lx cr2=%016lx -> SIGSEGV\n",
                exc_name(r->vector), t ? t->id : -1, t ? t->name : "?",
                r->error_code, r->rip, cr2);
        task_exit_user(139);            /* 128 + SIGSEGV; reported via wait4 */
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

    panic("unhandled exception");
}
