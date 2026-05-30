#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "task.h"
#include "file.h"
#include "pipe.h"
#include "fs.h"
#include "timer.h"
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
#define EBADF_      9
#define ENOMEM_     12
#define ENOTDIR_    20
#define EISDIR_     21
#define EINVAL_     22
#define ENFILE_     23
#define EMFILE_     24
#define ENOTTY_     25
#define ENOSPC_     28
#define E2BIG_      7
#define ENOSYS_     38
#define EFAULT_     14

/* The current user task's address space — the target for all copy_*_user
   validation. Kernel threads have no vm and never reach the user-pointer paths. */
static inline vmspace_t *cur_vm(void) { return task_current()->vm; }

/* Linux open() flags (subset). */
#define O_WRONLY      0x1
#define O_RDWR        0x2
#define O_CREAT       0x40
#define O_TRUNC       0x200
#define O_APPEND      0x400
#define O_DIRECTORY   0x10000

/* *at() dirfd / flags. */
#define AT_FDCWD       (-100)
#define AT_EMPTY_PATH  0x1000

/* lseek whence. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* st_mode type bits + dirent d_type. */
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define DT_DIR   4
#define DT_REG   8

/* Map a VibeFS FS_* error (negative) to a Linux errno (negative). */
static int64_t fs_to_errno(int e) {
    switch (e) {
    case FS_ENOENT:      return -ENOENT_;
    case FS_EEXIST:      return -17;
    case FS_ENOTDIR:     return -ENOTDIR_;
    case FS_EISDIR:      return -EISDIR_;
    case FS_ENOSPC:      return -ENOSPC_;
    case FS_EINVAL:      return -EINVAL_;
    case FS_ENOTEMPTY:   return -39;
    case FS_EMFILE:      return -EMFILE_;
    case FS_EBADF:       return -EBADF_;
    case FS_ENAMETOOLONG:return -36;
    default:             return -EINVAL_;
    }
}

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

/* Linux x86-64 struct stat (144 bytes; field offsets must be exact). */
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;  int64_t st_atime_nsec;
    int64_t  st_mtime;  int64_t st_mtime_nsec;
    int64_t  st_ctime;  int64_t st_ctime_nsec;
    int64_t  __unused[3];
};

/* Linux getdents64 record; d_name (NUL-terminated) follows inline. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

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

/* ---- per-process fd helpers ---- */

/* Resolve a user fd to its open-file object, or NULL if it is out of range,
   closed, or one of the implicit console fds (0/1/2). */
static file_t *fd_get(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD) return nullptr;
    return task_current()->fdt[fd];
}

/* Install `f` at the lowest free descriptor >= 3 (0/1/2 are the console).
   Returns the fd, or -EMFILE if the table is full. */
static int fd_install(file_t *f) {
    task_t *t = task_current();
    for (int i = 3; i < VFS_MAX_FD; i++)
        if (!t->fdt[i]) { t->fdt[i] = f; return i; }
    return -EMFILE_;
}

/* ---- basic I/O / exit ---- */

static int64_t sys_write(int fd, const void *buf, uint64_t n) {
    if (n && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)buf, n, 0))
        return -EFAULT_;                        /* bad user buffer: no kernel fault */
    if (fd == 1 || fd == 2) {                   /* stdout / stderr -> serial */
        serial_write((const char *)buf, (size_t)n);
        return (int64_t)n;
    }
    if (fd == 0) return -EBADF_;
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    if (f->kind == FD_PIPE_WR) return pipe_write(f->pipe, buf, (uint32_t)n, f->flags);
    if (f->kind == FD_PIPE_RD) return -EBADF_;  /* write to read end */
    if (f->kind != FD_FILE) return -EISDIR_;
    if (f->flags & O_APPEND) {                  /* append: write at EOF */
        fs_stat_t st;
        if (fs_istat(f->ino, &st) == FS_OK) f->off = st.size;
    }
    int r = fs_pwrite(f->ino, f->off, buf, (uint32_t)n);
    if (r < 0) return fs_to_errno(r);
    f->off += (uint64_t)r;
    return r;
}

static int64_t sys_read(int fd, void *buf, uint64_t n) {
    if (n && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)buf, n, 1))
        return -EFAULT_;                        /* writable check + COW break */
    if (fd == 0)                                /* stdin -> serial console TTY */
        return tty_read((char *)buf, (uint32_t)n);
    if (fd == 1 || fd == 2) return -EBADF_;
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    if (f->kind == FD_PIPE_RD) return pipe_read(f->pipe, buf, (uint32_t)n, f->flags);
    if (f->kind == FD_PIPE_WR) return -EBADF_;  /* read from write end */
    if (f->kind != FD_FILE) return -EISDIR_;
    int r = fs_pread(f->ino, f->off, buf, (uint32_t)n);
    if (r < 0) return fs_to_errno(r);
    f->off += (uint64_t)r;
    return r;
}

__attribute__((noreturn))
static void sys_exit(int code) {
    task_exit_user(code);                       /* zombie + wake parent; no return */
    __builtin_unreachable();
}

/* Copy a NUL-terminated user string into a kernel buffer, validating the user
   pages as it goes. A bad pointer yields an empty string rather than a kernel
   fault (callers that care about the distinction use strncpy_from_user). */
static void copy_user_str(char *dst, const char *src, unsigned n) {
    if (strncpy_from_user(dst, cur_vm(), (uint64_t)(uintptr_t)src, n) < 0)
        dst[0] = '\0';
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
        for (; n < EXEC_MAX_ARGS; n++) {
            uint64_t sptr;                          /* read uvec[n] safely */
            if (copy_from_user(&sptr, cur_vm(),
                    (uint64_t)(uintptr_t)&uvec[n], sizeof sptr) < 0) return -1;
            if (sptr == 0) break;                   /* NULL terminator */
            kvec[n] = buf + off;
            long len = strncpy_from_user(buf + off, cur_vm(), sptr, bufsz - off);
            if (len < 0) return -1;                 /* fault or no room for the NUL */
            off += (unsigned)len + 1;
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
    case ARCH_GET_FS:
        if (copy_to_user(cur_vm(), addr, &t->fs_base, sizeof t->fs_base) < 0)
            return -EFAULT_;
        return 0;
    default:          return -EINVAL_;
    }
}

static int64_t sys_writev(int fd, const struct iovec *iov, int cnt) {
    if (cnt < 0) return -EINVAL_;
    if (cnt && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)iov,
                               (uint64_t)cnt * sizeof(struct iovec), 0))
        return -EFAULT_;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t r = sys_write(fd, iov[i].iov_base, iov[i].iov_len);  /* routes file/pipe/console */
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break;     /* short write (pipe full / EOF) */
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
        if (vmspace_query(t->vm, va, &pa)) paging_unref_page(pa);  /* COW-aware free */
    }
    vmspace_unmap(t->vm, addr, len / PAGE_SIZE);
    return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot) {
    len  = PAGE_ALIGN_UP(len);
    addr = PAGE_ALIGN_DOWN(addr);
    task_t *t = task_current();
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        uint64_t pa;
        if (!vmspace_query(t->vm, va, &pa)) continue;
        if (prot & PROT_WRITE) {
            paging_cow_fault(t->vm, va);            /* privatise if COW-shared */
            if (!vmspace_query(t->vm, va, &pa)) continue;  /* pa may have moved */
            vmspace_map(t->vm, va, pa, 1, PTE_P | PTE_U | PTE_W);
        } else {
            vmspace_map(t->vm, va, pa, 1, PTE_P | PTE_U);
        }
    }
    return 0;
}

/* ---- §4 rung 2: file I/O against VibeFS through the fd table ---- */

/* Build an absolute kernel-side path from a user path. Relative paths are taken
   from the (fixed) cwd "/", since there is no chdir yet. */
static void resolve_path(char *dst, unsigned dstsz, const char *upath) {
    char tmp[256];
    copy_user_str(tmp, upath, sizeof tmp);
    unsigned i = 0;
    if (tmp[0] != '/') { if (dstsz) dst[i++] = '/'; }
    for (unsigned j = 0; tmp[j] && i + 1 < dstsz; j++) dst[i++] = tmp[j];
    dst[i] = '\0';
}

/* Fill a Linux struct stat from a VibeFS inode. */
static void fill_stat(struct linux_stat *st, uint32_t ino, const fs_stat_t *s) {
    kmemset(st, 0, sizeof *st);
    st->st_dev   = 1;
    st->st_ino   = ino;
    st->st_nlink = s->links;
    int dir = (s->type == FT_DIR);
    st->st_mode  = (dir ? S_IFDIR | 0755 : S_IFREG | 0644);
    st->st_size  = (int64_t)s->size;
    st->st_blksize = FS_BLOCK_SIZE;
    st->st_blocks  = (int64_t)((s->size + 511) / 512);
    st->st_mtime = s->mtime / 100;   /* 100 Hz ticks -> seconds */
    st->st_ctime = s->ctime / 100;
    st->st_atime = s->mtime / 100;
}

/* A char-device stat for the console fds (0/1/2). */
static void fill_stat_console(struct linux_stat *st) {
    kmemset(st, 0, sizeof *st);
    st->st_dev = 1; st->st_ino = 0; st->st_nlink = 1;
    st->st_mode = S_IFCHR | 0620;
    st->st_blksize = 1024;
}

static int64_t sys_openat(int dirfd, const char *upath, int flags, int mode) {
    (void)dirfd; (void)mode;                    /* AT_FDCWD only; cwd is "/" */
    char path[256];
    resolve_path(path, sizeof path, upath);

    int ino = fs_resolve(path);
    if (ino < 0) {
        if ((flags & O_CREAT) && !(flags & O_DIRECTORY)) {
            int r = fs_create(path);
            if (r != FS_OK) return fs_to_errno(r);
            ino = fs_resolve(path);
            if (ino < 0) return fs_to_errno(ino);
        } else {
            return -ENOENT_;
        }
    }

    fs_stat_t s;
    if (fs_istat((uint32_t)ino, &s) != FS_OK) return -ENOENT_;
    int acc = flags & 3;

    fd_kind_t kind;
    if (s.type == FT_DIR) {
        if (acc != 0) return -EISDIR_;          /* dirs are read-only */
        kind = FD_DIR;
    } else {
        if (flags & O_DIRECTORY) return -ENOTDIR_;
        if ((flags & O_TRUNC) && acc != 0) {
            int r = fs_truncate_ino((uint32_t)ino);
            if (r != FS_OK) return fs_to_errno(r);
        }
        kind = FD_FILE;
    }

    file_t *f = file_alloc();
    if (!f) return -ENFILE_;
    f->kind = kind; f->ino = (uint32_t)ino; f->off = 0; f->flags = flags;
    int fd = fd_install(f);
    if (fd < 0) { file_unref(f); return fd; }
    return fd;
}

static int64_t sys_close(int fd) {
    if (fd >= 0 && fd <= 2) return 0;           /* console: no-op */
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    task_current()->fdt[fd] = nullptr;
    file_unref(f);
    return 0;
}

static int64_t sys_lseek(int fd, int64_t off, int whence) {
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    int64_t base = 0;
    if (whence == SEEK_SET)      base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)f->off;
    else if (whence == SEEK_END) {
        fs_stat_t s;
        if (fs_istat(f->ino, &s) != FS_OK) return -EINVAL_;
        base = (int64_t)s.size;
    } else return -EINVAL_;
    int64_t n = base + off;
    if (n < 0) return -EINVAL_;
    f->off = (uint64_t)n;
    return n;
}

/* Fill `out` (a kernel struct) for an fd; the caller copies it to user space. */
static int64_t fstat_k(int fd, struct linux_stat *out) {
    if (fd >= 0 && fd <= 2) { fill_stat_console(out); return 0; }
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    if (f->kind == FD_PIPE_RD || f->kind == FD_PIPE_WR) {
        kmemset(out, 0, sizeof *out);
        out->st_mode = S_IFIFO | 0600;
        out->st_nlink = 1;
        out->st_blksize = 4096;
        return 0;
    }
    fs_stat_t s;
    if (fs_istat(f->ino, &s) != FS_OK) return -ENOENT_;
    fill_stat(out, f->ino, &s);
    return 0;
}

static int64_t sys_fstat(int fd, struct linux_stat *st) {
    struct linux_stat ks;
    int64_t r = fstat_k(fd, &ks);
    if (r < 0) return r;
    if (copy_to_user(cur_vm(), (uint64_t)(uintptr_t)st, &ks, sizeof ks) < 0) return -EFAULT_;
    return 0;
}

/* Fill `out` for a path; the caller copies it to user space. */
static int64_t pathstat_k(const char *upath, struct linux_stat *out) {
    char path[256];
    resolve_path(path, sizeof path, upath);
    int ino = fs_resolve(path);
    if (ino < 0) return fs_to_errno(ino);
    fs_stat_t s;
    if (fs_istat((uint32_t)ino, &s) != FS_OK) return -ENOENT_;
    fill_stat(out, (uint32_t)ino, &s);
    return 0;
}

static int64_t sys_newfstatat(int dirfd, const char *upath,
                              struct linux_stat *st, int flag) {
    struct linux_stat ks;
    int64_t r;
    /* AT_EMPTY_PATH with an empty string means stat the dirfd itself. */
    if (flag & AT_EMPTY_PATH) {
        char tmp[4]; copy_user_str(tmp, upath, sizeof tmp);
        if (tmp[0] == '\0') r = fstat_k(dirfd, &ks);
        else                r = pathstat_k(upath, &ks);
    } else {
        r = pathstat_k(upath, &ks);
    }
    if (r < 0) return r;
    if (copy_to_user(cur_vm(), (uint64_t)(uintptr_t)st, &ks, sizeof ks) < 0) return -EFAULT_;
    return 0;
}

static int64_t sys_stat(const char *upath, struct linux_stat *st) {
    struct linux_stat ks;
    int64_t r = pathstat_k(upath, &ks);
    if (r < 0) return r;
    if (copy_to_user(cur_vm(), (uint64_t)(uintptr_t)st, &ks, sizeof ks) < 0) return -EFAULT_;
    return 0;
}

static int64_t sys_getdents64(int fd, void *ubuf, uint64_t count) {
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    if (f->kind != FD_DIR) return -ENOTDIR_;
    if (count && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)ubuf, count, 1))
        return -EFAULT_;

    uint8_t *out = (uint8_t *)ubuf;
    uint64_t used = 0;
    for (;;) {
        fs_dirent_t de;
        uint64_t saved = f->off;
        int r = fs_dirent_at(f->ino, &f->off, &de);
        if (r < 0) return fs_to_errno(r);
        if (r == 0) break;                      /* end of directory */

        unsigned namelen = (unsigned)kstrlen(de.name);
        unsigned reclen = (sizeof(struct linux_dirent64) + namelen + 1 + 7) & ~7u;
        if (used + reclen > count) {            /* doesn't fit: rewind, stop */
            f->off = saved;
            if (used == 0) return -EINVAL_;     /* buffer too small for one entry */
            break;
        }
        struct linux_dirent64 *d = (struct linux_dirent64 *)(out + used);
        d->d_ino = de.inode;
        d->d_off = (int64_t)f->off;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = (de.type == FT_DIR) ? DT_DIR : DT_REG;
        kmemcpy(d->d_name, de.name, namelen + 1);
        used += reclen;
    }
    return (int64_t)used;
}

static int64_t sys_getcwd(char *ubuf, uint64_t size) {
    const char *cwd = "/";                       /* no chdir yet */
    uint64_t n = (uint64_t)kstrlen(cwd) + 1;
    if (size < n) return -34;                    /* -ERANGE */
    if (copy_to_user(cur_vm(), (uint64_t)(uintptr_t)ubuf, cwd, n) < 0) return -EFAULT_;
    return (int64_t)n;
}

/* pipe2: create a pipe and install its read/write ends at the two lowest free
   descriptors, writing them to ufds[0]/ufds[1]. O_NONBLOCK/O_CLOEXEC in flags
   are honored as far as we model them (CLOEXEC is a no-op today). */
static int64_t sys_pipe2(int *ufds, int flags) {
    if (!paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)ufds, 2 * sizeof(int), 1))
        return -EFAULT_;                         /* validate before allocating */
    pipe_t *p = pipe_create();
    if (!p) return -ENOMEM_;

    file_t *rf = file_alloc();
    file_t *wf = file_alloc();
    if (!rf || !wf) {
        /* Neither end is attached to the pipe yet (kind still FD_NONE), so
           file_unref just frees the slot — the pipe must be freed directly. */
        if (rf) file_unref(rf);
        if (wf) file_unref(wf);
        pipe_free(p);
        return -ENFILE_;
    }
    int extra = (flags & 04000) ? 04000 : 0;     /* O_NONBLOCK */
    rf->kind = FD_PIPE_RD; rf->pipe = p; rf->flags = extra;
    wf->kind = FD_PIPE_WR; wf->pipe = p; wf->flags = extra;

    int rfd = fd_install(rf);
    if (rfd < 0) { file_unref(rf); file_unref(wf); return rfd; }
    int wfd = fd_install(wf);
    if (wfd < 0) {
        task_current()->fdt[rfd] = nullptr; file_unref(rf);
        file_unref(wf);
        return wfd;
    }
    ufds[0] = rfd;
    ufds[1] = wfd;
    return 0;
}

static int64_t sys_pipe(int *ufds) { return sys_pipe2(ufds, 0); }

/* dup the open-file at `oldfd` into the lowest free descriptor >= 3. */
static int64_t sys_dup(int oldfd) {
    if (oldfd >= 0 && oldfd <= 2) return oldfd;  /* console fds are identity */
    file_t *f = fd_get(oldfd);
    if (!f) return -EBADF_;
    file_ref(f);
    int fd = fd_install(f);
    if (fd < 0) { file_unref(f); return fd; }
    return fd;
}

/* dup oldfd onto newfd exactly, closing whatever newfd held. */
static int64_t sys_dup2(int oldfd, int newfd) {
    if (oldfd >= 0 && oldfd <= 2 && newfd >= 0 && newfd <= 2) return newfd;
    file_t *f = fd_get(oldfd);
    if (!f) return -EBADF_;
    if (newfd < 0 || newfd >= VFS_MAX_FD || newfd <= 2) return -EBADF_;
    if (oldfd == newfd) return newfd;
    task_t *t = task_current();
    if (t->fdt[newfd]) file_unref(t->fdt[newfd]);
    file_ref(f);
    t->fdt[newfd] = f;
    return newfd;
}

/* Minimal fcntl: F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL. Enough for musl's
   stdio and isatty paths; FD_CLOEXEC is accepted but not enforced (execve keeps
   fds either way today). */
static int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    enum { F_DUPFD=0, F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4 };
    file_t *f = fd_get(fd);
    int is_console = (fd >= 0 && fd <= 2);
    if (!f && !is_console) return -EBADF_;
    switch (cmd) {
    case F_DUPFD: return sys_dup(fd);
    case F_GETFD: return 0;                       /* no FD_CLOEXEC tracking */
    case F_SETFD: return 0;
    case F_GETFL: return f ? f->flags : O_RDWR;
    case F_SETFL: if (f) f->flags = (int)arg; return 0;
    default:      return -EINVAL_;
    }
}

static int64_t sys_wait4(int pid, int *ustatus, int options, void *rusage) {
    (void)pid; (void)options; (void)rusage;     /* wait for any child for now */
    int code = 0;
    int got = task_wait(&code);
    if (got < 0) return got;                    /* -ECHILD */
    if (ustatus) {
        int wstatus = (code & 0xff) << 8;       /* WEXITSTATUS-compatible */
        if (copy_to_user(cur_vm(), (uint64_t)(uintptr_t)ustatus,
                         &wstatus, sizeof wstatus) < 0)
            return -EFAULT_;
    }
    return got;
}

extern "C" uint64_t syscall_dispatch(syscall_frame_t *f) {
    switch (f->rax) {
    case 0:   return (uint64_t)sys_read((int)f->rdi, (void *)f->rsi, f->rdx);
    case 1:   return (uint64_t)sys_write((int)f->rdi, (const void *)f->rsi, f->rdx);
    case 2:   return (uint64_t)sys_openat(AT_FDCWD, (const char *)f->rdi,
                                          (int)f->rsi, (int)f->rdx);   /* open */
    case 3:   return (uint64_t)sys_close((int)f->rdi);         /* close */
    case 4:   return (uint64_t)sys_stat((const char *)f->rdi,
                                        (struct linux_stat *)f->rsi);  /* stat */
    case 5:   return (uint64_t)sys_fstat((int)f->rdi, (struct linux_stat *)f->rsi); /* fstat */
    case 6:   return (uint64_t)sys_stat((const char *)f->rdi,
                                        (struct linux_stat *)f->rsi);  /* lstat (no symlinks) */
    case 8:   return (uint64_t)sys_lseek((int)f->rdi, (int64_t)f->rsi, (int)f->rdx); /* lseek */
    case 9:   return (uint64_t)sys_mmap(f->rdi, f->rsi, (int)f->rdx, (int)f->r10); /* mmap */
    case 10:  return (uint64_t)sys_mprotect(f->rdi, f->rsi, (int)f->rdx);  /* mprotect */
    case 11:  return (uint64_t)sys_munmap(f->rdi, f->rsi);      /* munmap */
    case 12:  return sys_brk(f->rdi);
    case 13:  return 0;                                        /* rt_sigaction (stub) */
    case 14:  return 0;                                        /* rt_sigprocmask (stub) */
    case 16:  return (uint64_t)(int64_t)-ENOTTY_;              /* ioctl: not a tty */
    case 20:  return (uint64_t)sys_writev((int)f->rdi, (const struct iovec *)f->rsi,
                                          (int)f->rdx);        /* writev */
    case 22:  return (uint64_t)sys_pipe((int *)f->rdi);        /* pipe */
    case 28:  return 0;                                        /* madvise (stub) */
    case 32:  return (uint64_t)sys_dup((int)f->rdi);           /* dup */
    case 33:  return (uint64_t)sys_dup2((int)f->rdi, (int)f->rsi);  /* dup2 */
    case 39:  return (uint64_t)task_current()->id;             /* getpid */
    case 72:  return (uint64_t)sys_fcntl((int)f->rdi, (int)f->rsi, f->rdx);  /* fcntl */
    case 186: return (uint64_t)task_current()->id;             /* gettid (== pid, no threads) */
    case 79:  return (uint64_t)sys_getcwd((char *)f->rdi, f->rsi);  /* getcwd */
    case 89:  return (uint64_t)(int64_t)-EINVAL_;              /* readlink: no symlinks */
    case 217: return (uint64_t)sys_getdents64((int)f->rdi, (void *)f->rsi, f->rdx); /* getdents64 */
    case 257: return (uint64_t)sys_openat((int)f->rdi, (const char *)f->rsi,
                                          (int)f->rdx, (int)f->r10);   /* openat */
    case 262: return (uint64_t)sys_newfstatat((int)f->rdi, (const char *)f->rsi,
                                              (struct linux_stat *)f->rdx, (int)f->r10); /* newfstatat */
    case 293: return (uint64_t)sys_pipe2((int *)f->rdi, (int)f->rsi);  /* pipe2 */
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
