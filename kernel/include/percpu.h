#ifndef VIBEOS_PERCPU_H
#define VIBEOS_PERCPU_H

/*
 * Per-CPU control block (ROADMAP §2).
 *
 * Each CPU owns a struct percpu reached through its GS base: the SYSCALL entry
 * stub does `swapgs` and then finds the kernel stack to switch to (and a scratch
 * slot for the user rsp) at fixed offsets via %gs. Each CPU also has its own TSS
 * (so a ring-3 -> ring-0 trap lands on the running task's kernel stack on *that*
 * CPU), which is what lets user tasks run on every core instead of just the BSP.
 *
 * The two offsets used from assembly (usermode.S) are fixed here and checked
 * against the C layout with a static_assert in percpu.c.
 */

#define PCPU_USER_SCRATCH  0    /* offsetof(struct percpu, user_scratch) */
#define PCPU_KERNEL_RSP    8    /* offsetof(struct percpu, kernel_rsp)   */

#ifndef __ASSEMBLER__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

struct percpu {
    uint64_t user_scratch;   /* +0:  syscall entry stashes the user rsp here   */
    uint64_t kernel_rsp;     /* +8:  kernel stack the syscall stub switches to */
    uint32_t index;          /* +16: CPU index (0 = BSP)                       */
    uint32_t _pad;
    tss_t    tss;            /* this CPU's TSS (rsp0 = current task's kstack)  */
};

/* Build CPU `idx`'s TSS descriptor in the shared GDT, load it (ltr), point this
   CPU's GS base at its percpu block. Call once per CPU after gdt_init. */
void percpu_init(int idx);

/* The calling CPU's percpu block. */
struct percpu *percpu_current(void);

/* Latch the kernel stack the running (user) task should use for syscalls and
   ring-3 traps on this CPU: updates both %gs:kernel_rsp and tss.rsp0. */
void percpu_set_kernel_stack(uint64_t top);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */
#endif
