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
/* syscall_frame_t lives in usermode.h now (fork clones it too). */

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

/* ---- per-process brk heap ---- */

void user_heap_init(uint64_t start, uint64_t max) {
    task_t *t = task_current();
    t->brk_start = t->brk_cur = start;
    t->brk_max   = max;
}

static uint64_t sys_brk(uint64_t newbrk) {
    task_t *t = task_current();
    if (newbrk == 0 || newbrk < t->brk_start || newbrk > t->brk_max)
        return t->brk_cur;                      /* query / out of range */
    uint64_t old_pg = PAGE_ALIGN_UP(t->brk_cur);
    uint64_t new_pg = PAGE_ALIGN_UP(newbrk);
    for (uint64_t va = old_pg; va < new_pg; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) return t->brk_cur;             /* leave break unchanged */
        vmspace_map(t->vm, va, pa, 1, PTE_P | PTE_W | PTE_U);
    }
    t->brk_cur = newbrk;
    return t->brk_cur;
}

/* ---- fork ---- */

static int64_t sys_fork(syscall_frame_t *f) {
    task_t *parent = task_current();
    if (!parent->vm) return -38;                /* kernel thread can't fork */
    vmspace_t *cvm = vmspace_fork(parent->vm);
    if (!cvm) return -12;                        /* -ENOMEM */
    task_t *child = task_fork("user", cvm, f);
    if (!child) { vmspace_destroy(cvm); return -11; }  /* -EAGAIN */
    return child->id;                            /* parent gets child's pid */
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
    task_exit_user(code);                       /* zombie + wake parent; no return */
    __builtin_unreachable();
}

/* Copy a NUL-terminated user string into a kernel buffer (must run while the
   caller's address space is still active). No fault handling yet. */
static void copy_user_str(char *dst, const char *src, unsigned n) {
    unsigned i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Replace the current process image with the program named by `upath`
   (resolved against the embedded program table for now). Builds a fresh
   address space, and on success never returns — it enters the new image. */
static int64_t sys_execve(const char *upath) {
    char path[64];
    copy_user_str(path, upath, sizeof path);    /* old address space still active */

    uint64_t size;
    const void *img = prog_lookup(path, &size);
    if (!img) return -2;                        /* -ENOENT */

    task_t *t = task_current();
    vmspace_t *oldvm = t->vm;
    vmspace_t *newvm = vmspace_create();
    if (!newvm) return -12;                     /* -ENOMEM */

    task_set_vmspace(newvm);                    /* CR3 = newvm for the load */
    uint64_t entry, rsp;
    int r = user_load(newvm, img, size, path, &entry, &rsp);
    if (r < 0) {                                /* bad image: keep old one */
        task_set_vmspace(oldvm);
        vmspace_destroy(newvm);
        return -8;                              /* -ENOEXEC */
    }
    vmspace_destroy(oldvm);                     /* committed; old AS now idle */
    kprintf("[syscall] exec '%s' -> entry=%lx\n", path, (unsigned long)entry);
    enter_user(entry, rsp);                     /* does not return */
}

static int64_t sys_wait4(int pid, int *ustatus, int options, void *rusage) {
    (void)pid; (void)options; (void)rusage;     /* wait for any child for now */
    int code = 0;
    int got = task_wait(&code);
    if (got < 0) return got;                    /* -ECHILD */
    if (ustatus) *ustatus = (code & 0xff) << 8; /* WEXITSTATUS-compatible */
    return got;
}

extern "C" uint64_t syscall_dispatch(syscall_frame_t *f) {
    switch (f->rax) {
    case 0:   return (uint64_t)sys_read((int)f->rdi, (void *)f->rsi, f->rdx);
    case 1:   return (uint64_t)sys_write((int)f->rdi, (const void *)f->rsi, f->rdx);
    case 12:  return sys_brk(f->rdi);
    case 39:  return (uint64_t)task_current()->id;             /* getpid */
    case 57:  return (uint64_t)sys_fork(f);                    /* fork */
    case 59:  return (uint64_t)sys_execve((const char *)f->rdi);  /* execve */
    case 61:  return (uint64_t)sys_wait4((int)f->rdi, (int *)f->rsi,
                                         (int)f->rdx, (void *)f->r10);  /* wait4 */
    case 60:                                                   /* exit */
    case 231: sys_exit((int)f->rdi);                           /* exit_group */
    default:
        kprintf("[syscall] unknown nr=%lu\n", (unsigned long)f->rax);
        return (uint64_t)-38;                   /* -ENOSYS */
    }
}
