#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "task.h"
#include "tty.h"
#include "usermode.h"

/*
 * System calls (ROADMAP §3 + §4 Linux ABI).
 *
 * syscall_init programs the SYSCALL/SYSRET MSRs; syscall_entry (usermode.S)
 * is the trap stub that marshals a syscall_frame_t and calls syscall_dispatch
 * below. Numbers follow the Linux x86_64 ABI so a cross-compiled static musl
 * binary runs unmodified.
 *
 * §4 widens the table enough to start a static musl program (TLS via
 * arch_prctl, anonymous mmap, vectored writev, plus a handful of startup
 * stubs) and plumbs argv/envp through execve.
 *
 * Simplifications still to revisit:
 *   - user pointers are dereferenced directly, with no copy_to/from_user
 *     validation (a bad pointer faults in the kernel);
 *   - fds 0/1/2 are hardwired to the console (a real fd table is §4 rung 2).
 */

/* Stack the syscall stub switches to, and its scratch for the user rsp.
   g_kernel_rsp is declared in usermode.h; both are touched from usermode.S. */
extern "C" uint64_t g_kernel_rsp = 0;
extern "C" uint64_t g_user_rsp   = 0;

extern "C" void syscall_entry(void);

/* ---- MSRs ---- */

#define MSR_EFER    0xC0000080u
#define MSR_STAR    0xC0000081u
#define MSR_LSTAR   0xC0000082u
#define MSR_SFMASK  0xC0000084u
#define MSR_FS_BASE 0xC0000100u
#define EFER_SCE    (1u << 0)

/* errno values returned to userspace (negated). */
#define ENOENT_     2
#define E2BIG_      7
#define EBADF_      9
#define ENOMEM_     12
#define EINVAL_     22
#define ENOTTY_     25
#define ENOSYS_     38

/* arch_prctl codes. */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* mmap prot / flags (subset). */
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

struct iovec { const void *iov_base; uint64_t iov_len; };

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

/* ---- basic I/O / exit ---- */

static int64_t sys_write(int fd, const void *buf, uint64_t n) {
    if (fd == 1 || fd == 2) {                   /* stdout / stderr -> serial */
        serial_write((const char *)buf, (size_t)n);
        return (int64_t)n;
    }
    return -EBADF_;
}

static int64_t sys_read(int fd, void *buf, uint64_t n) {
    if (fd == 0)                                /* stdin -> serial console TTY */
        return tty_read((char *)buf, (uint32_t)n);
    return -EBADF_;
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

/* ---- execve (argv/envp pass-through) ---- */

/* Kernel-side snapshot of an execve argument vector. argv/envp pointers index
   into `buf`; sized to comfortably cover a shell command line. */
#define EXEC_MAX_ARGS   64
#define EXEC_ARG_BYTES  16384
typedef struct {
    char *argv[EXEC_MAX_ARGS + 1];
    char *envp[EXEC_MAX_ARGS + 1];
    char  buf[EXEC_ARG_BYTES];
} exec_args_t;

/* Copy a user pointer-vector (NULL-terminated array of C strings) into the
   kernel buffer, appending strings after `*used`. Returns the element count, or
   -1 if the buffer would overflow. Runs while the caller's AS is still active. */
static int copy_user_vec(char *const uvec[], char **kvec,
                         char *buf, unsigned bufsz, unsigned *used) {
    unsigned off = *used;
    int n = 0;
    if (uvec) {
        for (; n < EXEC_MAX_ARGS && uvec[n]; n++) {
            const char *s = uvec[n];
            kvec[n] = buf + off;
            unsigned i = 0;
            while (off + 1 < bufsz && s[i]) buf[off++] = s[i++];
            if (off + 1 >= bufsz) return -1;        /* no room for the NUL */
            buf[off++] = '\0';
        }
    }
    kvec[n] = nullptr;
    *used = off;
    return n;
}

/* Replace the current process image with the program at `upath`, read from the
   mounted filesystem, passing argv/envp through to the new System V stack.
   Builds a fresh address space; on success never returns — it enters the new
   image. On failure the old image is kept intact. */
static int64_t sys_execve(const char *upath, char *const uargv[], char *const uenvp[]) {
    char path[128];
    copy_user_str(path, upath, sizeof path);    /* old address space still active */

    exec_args_t *a = (exec_args_t *)kmalloc(sizeof *a);
    if (!a) return -ENOMEM_;
    unsigned used = 0;
    if (copy_user_vec(uargv, a->argv, a->buf, sizeof a->buf, &used) < 0 ||
        copy_user_vec(uenvp, a->envp, a->buf, sizeof a->buf, &used) < 0) {
        kfree(a); return -E2BIG_;
    }
    if (!a->argv[0]) { a->argv[0] = path; a->argv[1] = nullptr; }   /* synthesize argv[0] */

    task_t *t = task_current();
    vmspace_t *oldvm = t->vm;
    vmspace_t *newvm = vmspace_create();
    if (!newvm) { kfree(a); return -ENOMEM_; }

    task_set_vmspace(newvm);                    /* CR3 = newvm for the load */
    uint64_t entry, rsp;
    int r = user_load_path(newvm, path, a->argv, a->envp, &entry, &rsp);
    if (r < 0) {                                /* not found / bad image: keep old */
        task_set_vmspace(oldvm);
        vmspace_destroy(newvm);
        kfree(a);
        return r;
    }
    vmspace_destroy(oldvm);                     /* committed; old AS now idle */
    kfree(a);
    t->fs_base = 0;                             /* fresh image: TLS reset until arch_prctl */
    kprintf("[syscall] exec '%s' -> entry=%lx\n", path, (unsigned long)entry);
    enter_user(entry, rsp);                     /* does not return */
}

/* ---- §4 Linux ABI: TLS, vectored I/O, anonymous mmap, startup stubs ---- */

/* arch_prctl: musl sets the thread pointer here (ARCH_SET_FS -> FS_BASE),
   saved per-task so it survives context switches (restored in apply_task_mm). */
static int64_t sys_arch_prctl(int code, uint64_t addr) {
    task_t *t = task_current();
    switch (code) {
    case ARCH_SET_FS: t->fs_base = addr; wrmsr(MSR_FS_BASE, addr); return 0;
    case ARCH_GET_FS: *(uint64_t *)(uintptr_t)addr = t->fs_base; return 0;
    default:          return -EINVAL_;
    }
}

static int64_t sys_writev(int fd, const struct iovec *iov, int cnt) {
    if (fd != 1 && fd != 2) return -EBADF_;     /* rung 2 widens to the fd table */
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len) serial_write((const char *)iov[i].iov_base, iov[i].iov_len);
        total += (int64_t)iov[i].iov_len;
    }
    return total;
}

/* Anonymous mmap arena: a per-process bump allocator in the user half. File
   backing (BFD, dynamic loaders) is a rung-3 follow-on. */
static int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags) {
    if (len == 0) return -EINVAL_;
    len = PAGE_ALIGN_UP(len);
    if (!(flags & MAP_ANONYMOUS)) return -ENOSYS_;   /* file-backed: §4 rung 3 */

    task_t *t = task_current();
    uint64_t base = ((flags & MAP_FIXED) && addr) ? PAGE_ALIGN_DOWN(addr)
                                                  : (t->mmap_next += len) - len;
    if (prot == 0) return (int64_t)base;             /* PROT_NONE: reserve VA only */

    uint64_t pflags = PTE_P | PTE_U | ((prot & PROT_WRITE) ? PTE_W : 0);
    for (uint64_t va = base; va < base + len; va += PAGE_SIZE) {
        uint64_t pa;
        if (vmspace_query(t->vm, va, &pa)) continue; /* fixed re-map over live page */
        pa = pmm_alloc_page();
        if (!pa) return -ENOMEM_;
        vmspace_map(t->vm, va, pa, 1, pflags);
    }
    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len) {
    if (len == 0) return -EINVAL_;
    len  = PAGE_ALIGN_UP(len);
    addr = PAGE_ALIGN_DOWN(addr);
    task_t *t = task_current();
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        uint64_t pa;
        if (vmspace_query(t->vm, va, &pa)) pmm_free_page(pa);
    }
    vmspace_unmap(t->vm, addr, len / PAGE_SIZE);
    return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot) {
    len  = PAGE_ALIGN_UP(len);
    addr = PAGE_ALIGN_DOWN(addr);
    task_t *t = task_current();
    uint64_t pflags = PTE_P | PTE_U | ((prot & PROT_WRITE) ? PTE_W : 0);
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        uint64_t pa;
        if (vmspace_query(t->vm, va, &pa)) vmspace_map(t->vm, va, pa, 1, pflags);
    }
    return 0;
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
    case 9:   return (uint64_t)sys_mmap(f->rdi, f->rsi, (int)f->rdx, (int)f->r10); /* mmap */
    case 10:  return (uint64_t)sys_mprotect(f->rdi, f->rsi, (int)f->rdx);  /* mprotect */
    case 11:  return (uint64_t)sys_munmap(f->rdi, f->rsi);      /* munmap */
    case 12:  return sys_brk(f->rdi);
    case 13:  return 0;                                        /* rt_sigaction (stub) */
    case 14:  return 0;                                        /* rt_sigprocmask (stub) */
    case 16:  return (uint64_t)(int64_t)-ENOTTY_;              /* ioctl: not a tty */
    case 20:  return (uint64_t)sys_writev((int)f->rdi, (const struct iovec *)f->rsi,
                                          (int)f->rdx);        /* writev */
    case 28:  return 0;                                        /* madvise (stub) */
    case 39:  return (uint64_t)task_current()->id;             /* getpid */
    case 57:  return (uint64_t)sys_fork(f);                    /* fork */
    case 59:  return (uint64_t)sys_execve((const char *)f->rdi, (char *const *)f->rsi,
                                          (char *const *)f->rdx);  /* execve */
    case 61:  return (uint64_t)sys_wait4((int)f->rdi, (int *)f->rsi,
                                         (int)f->rdx, (void *)f->r10);  /* wait4 */
    case 158: return (uint64_t)sys_arch_prctl((int)f->rdi, f->rsi);  /* arch_prctl */
    case 218: return (uint64_t)task_current()->id;             /* set_tid_address -> tid */
    case 60:                                                   /* exit */
    case 231: sys_exit((int)f->rdi);                           /* exit_group */
    default:
        kprintf("[syscall] unknown nr=%lu\n", (unsigned long)f->rax);
        return (uint64_t)-ENOSYS_;
    }
}
