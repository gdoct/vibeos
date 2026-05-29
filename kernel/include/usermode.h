#ifndef MYOS_USERMODE_H
#define MYOS_USERMODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ring-3 support (ROADMAP §3, Phase 1).
 *
 * tss_init    — install the TSS descriptor into the GDT and ltr it. Must run
 *               after gdt_init. The TSS exists so that an interrupt taken in
 *               ring 3 switches to a known kernel stack (TSS.rsp0).
 * tss_set_rsp0 — point rsp0 at the kernel stack the CPU should use on the next
 *               ring 3 -> ring 0 transition. The scheduler calls this on every
 *               switch so it tracks the running task's kernel stack.
 * syscall_init — enable SYSCALL/SYSRET and program STAR/LSTAR/SFMASK.
 * enter_user   — drop to ring 3 at `entry` with stack `user_rsp` (does not
 *               return). Also latches the kernel stack used for syscalls.
 */
void tss_init(void);
void tss_set_rsp0(uint64_t rsp0);

void syscall_init(void);

/* Reserve the user heap window for brk(). Called by the ELF loader once it
   knows where the image ends. */
void user_heap_init(uint64_t start, uint64_t max);

/* Load a static ET_EXEC x86_64 ELF held in memory into the current address
   space (low half, user pages) and build the System V initial stack. On
   success returns 0 and fills the entry point and the initial user rsp.
   Negative on a malformed/unsupported image. */
int user_load(const void *image, uint64_t size, uint64_t *entry_out, uint64_t *rsp_out);

__attribute__((noreturn))
void enter_user(uint64_t entry, uint64_t user_rsp);

/* Kernel stack the syscall entry stub switches to (top, grows down). Kept in
   sync with tss.rsp0 by the scheduler; on a UP kernel a global is enough
   (SMP §1 will move this to per-CPU GS, see syscall.S). */
extern uint64_t g_kernel_rsp;

#ifdef __cplusplus
}
#endif

#endif
