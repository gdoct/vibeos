#ifndef VIBEOS_USERMODE_H
#define VIBEOS_USERMODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vmspace;   /* kernel/include/paging.h */

/* The user register state captured by syscall_entry (usermode.S), laid out to
   match its push order. Defined here so both the syscall dispatcher and fork
   (which clones it onto the child's kernel stack) can see it. */
typedef struct syscall_frame {
    uint64_t rax;                          /* syscall number / return value */
    uint64_t rdi, rsi, rdx, r10, r8, r9;   /* args 1..6 */
    uint64_t rcx;                          /* user return rip */
    uint64_t r11;                          /* user rflags */
    uint64_t user_rsp;
    /* Callee-saved user regs. A normal syscall preserves these via the C ABI,
       so the return path leaves them alone — but fork must clone them into the
       child (whose kernel context would otherwise hand back zeros), or the
       child resumes with a corrupt rbp/rbx/r12-r15 (e.g. losing argv). */
    uint64_t rbx, rbp, r12, r13, r14, r15;
} syscall_frame_t;

/* Tail that a freshly-forked child's kernel stack returns into: pops a
   syscall_frame_t and sysrets to ring 3 with the child's rax = 0 (usermode.S). */
void fork_child_return(void);

/*
 * Ring-3 support (ROADMAP §3, Phase 1; per-CPU since §2).
 *
 * The per-CPU TSS (rsp0) and GS base live in percpu.c now — see percpu_init /
 * percpu_set_kernel_stack. This header keeps the syscall + ring-3 launch API.
 *
 * syscall_init — enable SYSCALL/SYSRET and program STAR/LSTAR/SFMASK.
 * enter_user   — drop to ring 3 at `entry` with stack `user_rsp` (does not
 *               return). swapgs's so the next syscall finds the kernel GS base.
 */
void syscall_init(void);

/* Reserve the user heap window for brk(). Called by the ELF loader once it
   knows where the image ends. */
void user_heap_init(uint64_t start, uint64_t max);

/* Read a static ET_EXEC x86_64 ELF from the mounted filesystem at `path` and
   load it into address space `vm` (low half, user pages), building the System V
   initial stack from the kernel-side `argv`/`envp` arrays (NULL-terminated) plus
   a real auxv. `vm` must be the active address space (CR3) so the loader can
   populate it through the user VAs. On success returns 0 and fills the entry
   point and initial user rsp; negative on FS error or a bad/unsupported image. */
int user_load_path(struct vmspace *vm, const char *path,
                   char *const argv[], char *const envp[],
                   uint64_t *entry_out, uint64_t *rsp_out);

__attribute__((noreturn))
void enter_user(uint64_t entry, uint64_t user_rsp);

#ifdef __cplusplus
}
#endif

#endif
