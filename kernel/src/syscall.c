#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "task.h"
#include "usermode.h"

/*
 * System calls (ROADMAP §3, Phases 1 + 3).
 *
 * syscall_init programs the SYSCALL/SYSRET MSRs; syscall_entry (syscall.S)
 * is the trap stub that marshals a syscall_frame_t and calls syscall_dispatch
 * below. Numbers follow the Linux x86_64 ABI so the same user binaries (and,
 * later, a static musl) keep working as §4 widens the table.
 *
 * Milestone A simplifications, to revisit:
 *   - user pointers are dereferenced directly (the single process shares the
 *     kernel's page tables), with no copy_to/from_user validation yet;
 *   - one global brk, since there is one process.
 */

/* Stack the syscall stub switches to, and its scratch for the user rsp.
   g_kernel_rsp is declared in usermode.h; both are touched from syscall.S. */
extern "C" uint64_t g_kernel_rsp = 0;
extern "C" uint64_t g_user_rsp   = 0;

extern "C" void syscall_entry(void);

/* Matches the push order in syscall.S (lowest address first). */
typedef struct {
    uint64_t rax;                          /* syscall number */
    uint64_t rdi, rsi, rdx, r10, r8, r9;   /* args 1..6 */
    uint64_t rcx;                          /* user return rip  */
    uint64_t r11;                          /* user rflags      */
    uint64_t user_rsp;
} syscall_frame_t;

/* ---- MSRs ---- */

#define MSR_EFER    0xC0000080u
#define MSR_STAR    0xC0000081u
#define MSR_LSTAR   0xC0000082u
#define MSR_SFMASK  0xC0000084u
#define EFER_SCE    (1u << 0)

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)));
}

void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* STAR[47:32] = SYSCALL CS base: kernel CS = 0x08, kernel SS = 0x10.
       STAR[63:48] = SYSRET base 0x10: user SS = 0x10+8 = 0x18, user CS =
       0x10+16 = 0x20 (both OR'd with RPL 3 by the CPU). Matches the GDT in
       gdt.S. */
    wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    /* Bits to clear from RFLAGS on entry: IF (no reentrancy on the user
       stack), DF (defined direction), TF. */
    wrmsr(MSR_SFMASK, (1u << 9) | (1u << 10) | (1u << 8));

    kprintf("[syscall] SYSCALL/SYSRET enabled (entry=%p)\n",
            (void *)(uintptr_t)syscall_entry);
}

/* ---- minimal brk heap (one process, Milestone A) ---- */

static uint64_t g_brk_start = 0;
static uint64_t g_brk_cur   = 0;
static uint64_t g_brk_max   = 0;

void user_heap_init(uint64_t start, uint64_t max) {
    g_brk_start = g_brk_cur = start;
    g_brk_max   = max;
}

static uint64_t sys_brk(uint64_t newbrk) {
    if (newbrk == 0 || newbrk < g_brk_start || newbrk > g_brk_max)
        return g_brk_cur;                       /* query / out of range */
    uint64_t old_pg = PAGE_ALIGN_UP(g_brk_cur);
    uint64_t new_pg = PAGE_ALIGN_UP(newbrk);
    for (uint64_t va = old_pg; va < new_pg; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) return g_brk_cur;              /* leave break unchanged */
        vmap(va, pa, 1, PTE_P | PTE_W | PTE_U);
    }
    g_brk_cur = newbrk;
    return g_brk_cur;
}

/* ---- syscalls ---- */

static int64_t sys_write(int fd, const void *buf, uint64_t n) {
    if (fd == 1 || fd == 2) {                   /* stdout / stderr -> serial */
        serial_write((const char *)buf, (size_t)n);
        return (int64_t)n;
    }
    return -9;                                  /* -EBADF */
}

static int64_t sys_read(int fd, void *buf, uint64_t n) {
    (void)fd; (void)buf; (void)n;
    return 0;                                   /* no console input yet -> EOF */
}

__attribute__((noreturn))
static void sys_exit(int code) {
    task_t *t = task_current();
    kprintf("[syscall] task %d \"%s\" exit(%d)\n", t->id, t->name, code);
    task_exit();                                /* does not return */
    __builtin_unreachable();
}

extern "C" uint64_t syscall_dispatch(syscall_frame_t *f) {
    switch (f->rax) {
    case 0:   return (uint64_t)sys_read((int)f->rdi, (void *)f->rsi, f->rdx);
    case 1:   return (uint64_t)sys_write((int)f->rdi, (const void *)f->rsi, f->rdx);
    case 12:  return sys_brk(f->rdi);
    case 39:  return (uint64_t)task_current()->id;             /* getpid */
    case 60:                                                   /* exit */
    case 231: sys_exit((int)f->rdi);                           /* exit_group */
    default:
        kprintf("[syscall] unknown nr=%lu\n", (unsigned long)f->rax);
        return (uint64_t)-38;                   /* -ENOSYS */
    }
}
