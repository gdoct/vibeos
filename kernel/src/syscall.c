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
#include "smp.h"
#include "signal.h"
#include "net.h"
#include "synth.h"
#include "csprng.h"

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

/* The kernel stack the syscall stub switches to and its user-rsp scratch now
   live in the per-CPU block (percpu.h), reached via %gs after swapgs in
   usermode.S — so user tasks run on any core (ROADMAP §2). */

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
#define EPIPE_      32
#define EACCES_     13
#define EEXIST_     17

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
#define O_CLOEXEC     0x80000

/* *at() dirfd / flags. */
#define AT_FDCWD       (-100)
#define AT_EMPTY_PATH  0x1000
#define AT_SYMLINK_NOFOLLOW 0x100

/* lseek whence. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* st_mode type bits + dirent d_type. */
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define DT_DIR   4
#define DT_REG   8
#define DT_CHR   2

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
        if (!t->fdt[i]) { t->fdt[i] = f; t->fd_cloexec[i] = 0; return i; }
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
    if (f->kind == FD_SOCKET) { int r = ksock_send(f->sock, buf, (uint32_t)n); return r < 0 ? -EPIPE_ : r; }
    if (f->kind == FD_DEV || f->kind == FD_PROC) return synth_write(f, buf, (uint32_t)n);
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
    if (f->kind == FD_SOCKET) return ksock_recv(f->sock, buf, (uint32_t)n);
    if (f->kind == FD_DEV || f->kind == FD_PROC) return synth_read(f, buf, (uint32_t)n);
    if (f->kind == FD_DEVDIR) return -EISDIR_;
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
    /* Close descriptors marked FD_CLOEXEC (ROADMAP §4). */
    for (int i = 3; i < VFS_MAX_FD; i++) {
        if (t->fd_cloexec[i] && t->fdt[i]) {
            file_t *cf = t->fdt[i];
            if (cf->kind == FD_SOCKET && cf->refcount == 1) ksock_close(cf->sock);
            t->fdt[i] = nullptr;
            file_unref(cf);
        }
        t->fd_cloexec[i] = 0;
    }
    signals_exec(t);                            /* reset caught handlers to default */
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

/* mmap: anonymous arena + file-backed MAP_PRIVATE (ROADMAP §4, for ld-musl).
   File backing copies the file's bytes into freshly allocated private pages (no
   shared page cache yet); the tail past EOF stays zero (BSS). */
static int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                        int fd, uint64_t off) {
    if (len == 0) return -EINVAL_;
    uint64_t mlen = PAGE_ALIGN_UP(len);
    int anon = (flags & MAP_ANONYMOUS);

    task_t *t = task_current();
    uint64_t base = ((flags & MAP_FIXED) && addr) ? PAGE_ALIGN_DOWN(addr)
                                                  : (t->mmap_next += mlen) - mlen;
    if (anon && prot == 0) return (int64_t)base;     /* PROT_NONE: reserve VA only */

    file_t *fl = nullptr;
    if (!anon) {
        fl = fd_get(fd);
        if (!fl || fl->kind != FD_FILE) return -EBADF_;
    }

    /* Map the pages writable first so we can load file content; the requested
       protection is applied afterwards (e.g. a read-only text segment). */
    for (uint64_t va = base; va < base + mlen; va += PAGE_SIZE) {
        uint64_t pa;
        if (vmspace_query(t->vm, va, &pa)) continue; /* fixed re-map over live page */
        pa = pmm_alloc_page();
        if (!pa) return -ENOMEM_;
        vmspace_map(t->vm, va, pa, 1, PTE_P | PTE_W | PTE_U);
    }

    if (!anon) {                                     /* stream the file in */
        uint64_t done = 0;
        while (done < len) {
            uint32_t chunk = (len - done > 0x100000u) ? 0x100000u : (uint32_t)(len - done);
            int r = fs_pread(fl->ino, off + done, (void *)(uintptr_t)(base + done), chunk);
            if (r <= 0) break;                       /* EOF / error: leave the rest zero */
            done += (uint64_t)r;
        }
    }

    /* Apply the final protection (drop write for read-only/exec-only regions). */
    if (!(prot & PROT_WRITE)) {
        uint64_t pflags = PTE_P | PTE_U;
        for (uint64_t va = base; va < base + mlen; va += PAGE_SIZE) {
            uint64_t pa;
            if (vmspace_query(t->vm, va, &pa)) vmspace_map(t->vm, va, pa, 1, pflags);
        }
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

/* Normalize an absolute path in-place semantics: collapse "//", ".", and ".."
   into `dst`. `src` must already be absolute (leading '/'). */
static void normalize_abs(char *dst, unsigned dstsz, const char *src) {
    /* Stack of component start offsets within dst (dst always begins with '/'). */
    unsigned stack[64]; int sp = 0;
    unsigned o = 0;
    if (dstsz) dst[o++] = '/';
    unsigned i = 0;
    while (src[i]) {
        while (src[i] == '/') i++;                 /* skip separators */
        if (!src[i]) break;
        char comp[64]; unsigned c = 0;             /* read one component */
        while (src[i] && src[i] != '/' && c < sizeof comp - 1) comp[c++] = src[i++];
        comp[c] = '\0';
        while (src[i] && src[i] != '/') i++;        /* drop an over-long tail */
        if (comp[0] == '.' && comp[1] == '\0') continue;            /* "." */
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {  /* ".." */
            if (sp > 0) { o = stack[--sp]; }        /* pop a component */
            continue;
        }
        if (sp < (int)(sizeof stack / sizeof stack[0])) stack[sp++] = o;
        if (o > 1 && o + 1 < dstsz) dst[o++] = '/'; /* separator before non-first */
        for (unsigned k = 0; comp[k] && o + 1 < dstsz; k++) dst[o++] = comp[k];
    }
    if (o == 0 && dstsz) dst[o++] = '/';
    dst[o < dstsz ? o : dstsz - 1] = '\0';
}

/* Build an absolute, normalized kernel-side path from a user path, taking
   relative paths from the calling task's cwd (ROADMAP §4). */
static void resolve_path(char *dst, unsigned dstsz, const char *upath) {
    char tmp[256];
    copy_user_str(tmp, upath, sizeof tmp);
    char joined[512];
    unsigned o = 0;
    if (tmp[0] != '/') {                            /* relative: prefix cwd */
        const char *cwd = task_current()->cwd;
        for (unsigned k = 0; cwd[k] && o + 1 < sizeof joined; k++) joined[o++] = cwd[k];
        if (o == 0 || joined[o - 1] != '/') if (o + 1 < sizeof joined) joined[o++] = '/';
    }
    for (unsigned k = 0; tmp[k] && o + 1 < sizeof joined; k++) joined[o++] = tmp[k];
    joined[o] = '\0';
    normalize_abs(dst, dstsz, joined);
}

/* Fill a Linux struct stat from a VibeFS inode. */
static void fill_stat(struct linux_stat *st, uint32_t ino, const fs_stat_t *s) {
    kmemset(st, 0, sizeof *st);
    st->st_dev   = 1;
    st->st_ino   = ino;
    st->st_nlink = s->links;
    st->st_mode  = (s->type == FT_DIR)     ? (S_IFDIR | 0755)
                 : (s->type == FT_SYMLINK) ? (S_IFLNK | 0777)
                                           : (S_IFREG | 0644);
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
    (void)dirfd; (void)mode;                    /* AT_FDCWD / absolute (cwd-relative) */
    char path[256];
    resolve_path(path, sizeof path, upath);

    /* Synthetic /dev and /proc take precedence over the real fs (ROADMAP §4). */
    if (synth_classify(path, nullptr) != SYNTH_NONE) {
        file_t *sf = file_alloc();
        if (!sf) return -ENFILE_;
        int sr = synth_open(path, sf);
        if (sr < 0) { file_unref(sf); return sr == -2 ? -ENOENT_ : -ENOENT_; }
        sf->flags = flags;
        int sfd = fd_install(sf);
        if (sfd < 0) { file_unref(sf); return sfd; }
        if (flags & O_CLOEXEC) task_current()->fd_cloexec[sfd] = 1;
        return sfd;
    }

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
    if (flags & O_CLOEXEC) task_current()->fd_cloexec[fd] = 1;
    return fd;
}

static int64_t sys_close(int fd) {
    if (fd >= 0 && fd <= 2) return 0;           /* console: no-op */
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    if (f->kind == FD_SOCKET && f->refcount == 1) ksock_close(f->sock);  /* last ref */
    task_current()->fdt[fd] = nullptr;
    task_current()->fd_cloexec[fd] = 0;
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

/* Stat a synthetic /dev or /proc object (ROADMAP §4). */
static void fill_stat_synth(struct linux_stat *st, int is_dir, int is_char) {
    kmemset(st, 0, sizeof *st);
    st->st_dev = 1; st->st_nlink = 1; st->st_blksize = 4096;
    st->st_mode = is_dir ? (S_IFDIR | 0555)
                : is_char ? (S_IFCHR | 0666)
                          : (S_IFREG | 0444);
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
    if (f->kind == FD_DEV)    { fill_stat_synth(out, 0, 1); return 0; }
    if (f->kind == FD_DEVDIR) { fill_stat_synth(out, 1, 0); return 0; }
    if (f->kind == FD_PROC)   { fill_stat_synth(out, 0, 0); return 0; }
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

/* Fill `out` for a path; the caller copies it to user space. `follow` chooses
   stat (follow a trailing symlink) vs lstat (return the symlink itself). */
static int64_t pathstat_k2(const char *upath, struct linux_stat *out, int follow) {
    char path[256];
    resolve_path(path, sizeof path, upath);
    int sc = synth_classify(path, nullptr);
    if (sc != SYNTH_NONE) {                      /* /dev or /proc */
        int is_char = (path[1] == 'd');          /* "/dev/..." -> char device */
        fill_stat_synth(out, sc == SYNTH_DIR, sc == SYNTH_NODE && is_char);
        return 0;
    }
    int ino = follow ? fs_resolve(path) : fs_lresolve(path);
    if (ino < 0) return fs_to_errno(ino);
    fs_stat_t s;
    if (fs_istat((uint32_t)ino, &s) != FS_OK) return -ENOENT_;
    fill_stat(out, (uint32_t)ino, &s);
    return 0;
}
static int64_t pathstat_k(const char *upath, struct linux_stat *out) {
    return pathstat_k2(upath, out, 1);           /* default: follow symlinks */
}

/* mkdir(path, mode): create a directory (mode ignored — no perms yet). */
static int64_t sys_mkdir(const char *upath, int mode) {
    (void)mode;
    char path[256];
    resolve_path(path, sizeof path, upath);
    if (synth_classify(path, nullptr) != SYNTH_NONE) return -EEXIST_;
    int r = fs_mkdir(path);
    return r == FS_OK ? 0 : fs_to_errno(r);
}

/* symlink(target, linkpath): create a symbolic link (ROADMAP §4). */
static int64_t sys_symlink(const char *utarget, const char *ulink) {
    char target[256], link[256];
    copy_user_str(target, utarget, sizeof target);
    resolve_path(link, sizeof link, ulink);
    if (synth_classify(link, nullptr) != SYNTH_NONE) return -EACCES_;  /* /dev,/proc are read-only */
    int r = fs_symlink(target, link);
    return r == FS_OK ? 0 : fs_to_errno(r);
}

/* readlink(path, buf, bufsz): read a symlink's target (no NUL terminator). */
static int64_t sys_readlink(const char *upath, char *ubuf, uint64_t bufsz) {
    char path[256];
    resolve_path(path, sizeof path, upath);
    int ino = fs_lresolve(path);
    if (ino < 0) return fs_to_errno(ino);
    char tmp[256];
    int r = fs_readlink((uint32_t)ino, tmp, sizeof tmp);
    if (r < 0) return r == FS_EINVAL ? -EINVAL_ : fs_to_errno(r);
    uint32_t n = (uint32_t)r < bufsz ? (uint32_t)r : (uint32_t)bufsz;
    if (n && copy_to_user(cur_vm(), (uint64_t)(uintptr_t)ubuf, tmp, n) < 0) return -EFAULT_;
    return (int64_t)n;
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

static int64_t sys_lstat(const char *upath, struct linux_stat *st) {
    struct linux_stat ks;
    int64_t r = pathstat_k2(upath, &ks, 0);      /* do not follow a trailing symlink */
    if (r < 0) return r;
    if (copy_to_user(cur_vm(), (uint64_t)(uintptr_t)st, &ks, sizeof ks) < 0) return -EFAULT_;
    return 0;
}

/* access / faccessat / faccessat2: existence check (no permission model yet, so
   any mode against an existing path succeeds). */
static int64_t sys_access(const char *upath, int mode) {
    (void)mode;
    struct linux_stat ks;
    return pathstat_k(upath, &ks) < 0 ? -ENOENT_ : 0;
}

/* statx — the modern stat used by recent coreutils/busybox. Fill the basic-stats
   fields from the same data as stat. */
static int64_t sys_statx(int dirfd, const char *upath, int flags,
                         unsigned mask, uint64_t ubuf) {
    (void)mask;
    struct linux_stat ks;
    int64_t r;
    char tmp[4];
    copy_user_str(tmp, upath, sizeof tmp);
    if ((flags & AT_EMPTY_PATH) && tmp[0] == '\0') r = fstat_k(dirfd, &ks);
    else r = pathstat_k2(upath, &ks, !(flags & AT_SYMLINK_NOFOLLOW));
    if (r < 0) return r;

    struct statx_ts { int64_t sec; uint32_t nsec; int32_t pad; };
    struct statx {
        uint32_t mask, blksize; uint64_t attributes;
        uint32_t nlink, uid, gid; uint16_t mode, pad1; uint64_t ino, size, blocks;
        uint64_t attributes_mask;
        struct statx_ts atime, btime, ctime, mtime;
        uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
        uint64_t mnt_id; uint64_t pad2[13];
    } sx;
    kmemset(&sx, 0, sizeof sx);
    sx.mask = 0x7ff;                             /* STATX_BASIC_STATS */
    sx.blksize = (uint32_t)ks.st_blksize;
    sx.nlink = (uint32_t)ks.st_nlink;
    sx.uid = ks.st_uid; sx.gid = ks.st_gid;
    sx.mode = (uint16_t)ks.st_mode;
    sx.ino = ks.st_ino; sx.size = (uint64_t)ks.st_size; sx.blocks = (uint64_t)ks.st_blocks;
    sx.atime.sec = ks.st_atime; sx.mtime.sec = ks.st_mtime; sx.ctime.sec = ks.st_ctime;
    sx.dev_major = 0; sx.dev_minor = 1;
    if (copy_to_user(cur_vm(), ubuf, &sx, sizeof sx) < 0) return -EFAULT_;
    return 0;
}

static int64_t sys_getdents64(int fd, void *ubuf, uint64_t count) {
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    if (f->kind == FD_DEVDIR) {                  /* synthetic /dev or /proc */
        if (count && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)ubuf, count, 1))
            return -EFAULT_;
        uint8_t *out = (uint8_t *)ubuf;
        uint64_t used = 0;
        for (;;) {
            char nm[64]; int type = 1;
            if (!synth_readdir(f, (int)f->off, nm, sizeof nm, &type)) break;
            unsigned namelen = (unsigned)kstrlen(nm);
            unsigned reclen = (sizeof(struct linux_dirent64) + namelen + 1 + 7) & ~7u;
            if (used + reclen > count) { if (used == 0) return -EINVAL_; break; }
            struct linux_dirent64 *d = (struct linux_dirent64 *)(out + used);
            d->d_ino = 1 + (uint64_t)f->off;
            d->d_off = (int64_t)(f->off + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = (type == 2) ? DT_DIR : (type == 1 ? DT_REG : DT_CHR);
            kmemcpy(d->d_name, nm, namelen + 1);
            used += reclen;
            f->off++;
        }
        return (int64_t)used;
    }
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
    const char *cwd = task_current()->cwd;
    uint64_t n = (uint64_t)kstrlen(cwd) + 1;
    if (size < n) return -34;                    /* -ERANGE */
    if (copy_to_user(cur_vm(), (uint64_t)(uintptr_t)ubuf, cwd, n) < 0) return -EFAULT_;
    return (int64_t)n;
}

/* chdir: resolve to an absolute normalized path, verify it is a directory
   (real or synthetic), and store it as the task's cwd (ROADMAP §4). */
static int64_t sys_chdir(const char *upath) {
    char path[256];
    resolve_path(path, sizeof path, upath);
    int sc = synth_classify(path, nullptr);
    if (sc == SYNTH_NONE) {
        int ino = fs_resolve(path);
        if (ino < 0) return fs_to_errno(ino);
        fs_stat_t s;
        if (fs_istat((uint32_t)ino, &s) != FS_OK) return -ENOENT_;
        if (s.type != FT_DIR) return -ENOTDIR_;
    } else if (sc != SYNTH_DIR) {
        return -ENOTDIR_;
    }
    task_t *t = task_current();
    unsigned i = 0;
    for (; path[i] && i < sizeof t->cwd - 1; i++) t->cwd[i] = path[i];
    t->cwd[i] = '\0';
    return 0;
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
    t->fd_cloexec[newfd] = 0;                     /* dup2 clears CLOEXEC (POSIX) */
    return newfd;
}

/* fcntl: F_DUPFD(_CLOEXEC) / F_GETFD / F_SETFD / F_GETFL / F_SETFL. FD_CLOEXEC is
   now tracked per descriptor and enforced by execve (ROADMAP §4). */
static int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    enum { F_DUPFD=0, F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4,
           F_SETLK=6, F_SETLKW=7, F_GETLK=5, F_DUPFD_CLOEXEC=1030 };
    enum { FD_CLOEXEC_BIT = 1, F_UNLCK = 2 };
    file_t *f = fd_get(fd);
    int is_console = (fd >= 0 && fd <= 2);
    if (!f && !is_console) return -EBADF_;
    task_t *t = task_current();
    switch (cmd) {
    case F_DUPFD: return sys_dup(fd);
    case F_DUPFD_CLOEXEC: {
        int64_t nfd = sys_dup(fd);
        if (nfd >= 0) t->fd_cloexec[nfd] = 1;
        return nfd;
    }
    case F_GETFD: return (fd >= 0 && fd < VFS_MAX_FD && t->fd_cloexec[fd]) ? FD_CLOEXEC_BIT : 0;
    case F_SETFD:
        if (fd >= 0 && fd < VFS_MAX_FD) t->fd_cloexec[fd] = (arg & FD_CLOEXEC_BIT) ? 1 : 0;
        return 0;
    case F_GETFL: return f ? f->flags : O_RDWR;
    case F_SETFL: if (f) f->flags = (int)arg; return 0;
    /* Advisory record locks: no real locking yet, so report "unlocked" and let
       every lock attempt succeed (enough for binutils/dpkg-style fcntl locking). */
    case F_GETLK: { int16_t unlck = F_UNLCK; copy_to_user(cur_vm(), arg, &unlck, sizeof unlck); return 0; }
    case F_SETLK:
    case F_SETLKW: return 0;
    default:      return -EINVAL_;
    }
}

static int64_t sys_wait4(int pid, int *ustatus, int options, void *rusage) {
    (void)pid; (void)options; (void)rusage;     /* wait for any child for now */
    int code = 0;
    int got = task_wait(&code);                 /* code already encoded (exit/signal) */
    if (got < 0) return got;                    /* -ECHILD */
    if (ustatus &&
        copy_to_user(cur_vm(), (uint64_t)(uintptr_t)ustatus, &code, sizeof code) < 0)
        return -EFAULT_;
    return got;
}

/* ---- §5 sockets (BSD socket API over the §4 fd table) ---- */

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;       /* network order */
    uint32_t sin_addr;       /* network order */
    uint8_t  zero[8];
};

/* Resolve a socket fd to its kernel socket object. */
static void *sock_get(int fd) {
    file_t *f = fd_get(fd);
    return (f && f->kind == FD_SOCKET) ? f->sock : nullptr;
}

/* Copy in a user sockaddr_in, returning ip/port in host byte order. */
static int read_sockaddr(uint64_t uaddr, uint32_t ulen, uint32_t *ip, uint16_t *port) {
    if (ulen < sizeof(struct sockaddr_in)) return -1;
    struct sockaddr_in sa;
    if (copy_from_user(&sa, cur_vm(), uaddr, sizeof sa) < 0) return -1;
    *ip   = ntohl_(sa.sin_addr);
    *port = ntohs_(sa.sin_port);
    return 0;
}

/* Write a sockaddr_in back to user (for accept/recvfrom peer address). */
static void write_sockaddr(uint64_t uaddr, uint64_t ulen_ptr, uint32_t ip, uint16_t port) {
    if (!uaddr) return;
    struct sockaddr_in sa;
    kmemset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons_(port);
    sa.sin_addr = htonl_(ip);
    copy_to_user(cur_vm(), uaddr, &sa, sizeof sa);
    if (ulen_ptr) { uint32_t l = sizeof sa; copy_to_user(cur_vm(), ulen_ptr, &l, sizeof l); }
}

static int install_socket(void *ks) {
    file_t *f = file_alloc();
    if (!f) return -ENFILE_;
    f->kind = FD_SOCKET; f->sock = ks; f->flags = O_RDWR;
    int fd = fd_install(f);
    if (fd < 0) { f->kind = FD_NONE; file_unref(f); }
    return fd;
}

static int64_t sys_socket(int domain, int type, int proto) {
    (void)proto;
    if (domain != AF_INET) return -EINVAL_;
    void *ks = ksock_create(type & 0xFF);            /* mask SOCK_NONBLOCK/CLOEXEC */
    if (!ks) return -EINVAL_;
    int fd = install_socket(ks);
    if (fd < 0) ksock_close(ks);
    return fd;
}

static int64_t sys_bind(int fd, uint64_t uaddr, uint32_t ulen) {
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    uint32_t ip; uint16_t port;
    if (read_sockaddr(uaddr, ulen, &ip, &port) < 0) return -EINVAL_;
    return ksock_bind(ks, ip, port) == 0 ? 0 : -EINVAL_;
}

static int64_t sys_listen(int fd, int backlog) {
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    return ksock_listen(ks, backlog) == 0 ? 0 : -EINVAL_;
}

static int64_t sys_connect(int fd, uint64_t uaddr, uint32_t ulen) {
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    uint32_t ip; uint16_t port;
    if (read_sockaddr(uaddr, ulen, &ip, &port) < 0) return -EINVAL_;
    return ksock_connect(ks, ip, port) == 0 ? 0 : -111;   /* -ECONNREFUSED */
}

/* getsockname (peer=0) / getpeername (peer=1) (ROADMAP ABI widening). */
static int64_t sys_getsockname(int fd, uint64_t uaddr, uint64_t ulen_ptr, int peer) {
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    uint32_t ip; uint16_t port;
    if (ksock_getname(ks, peer, &ip, &port) < 0) return -EINVAL_;
    write_sockaddr(uaddr, ulen_ptr, ip, port);
    return 0;
}

/* getsockopt: zero the option value (covers SO_ERROR after connect, etc.). */
static int64_t sys_getsockopt(int fd, int level, int optname, uint64_t uval, uint64_t ulen_ptr) {
    (void)level; (void)optname;
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    uint32_t ulen = 0;
    if (ulen_ptr) copy_from_user(&ulen, cur_vm(), ulen_ptr, sizeof ulen);
    if (uval && ulen >= sizeof(int)) {
        int zero = 0;
        copy_to_user(cur_vm(), uval, &zero, sizeof zero);
        uint32_t four = sizeof(int);
        if (ulen_ptr) copy_to_user(cur_vm(), ulen_ptr, &four, sizeof four);
    }
    return 0;
}

static int64_t sys_accept(int fd, uint64_t uaddr, uint64_t ulen_ptr) {
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    uint32_t pip = 0; uint16_t pport = 0;
    void *ns = ksock_accept(ks, &pip, &pport);
    if (!ns) return -EINVAL_;
    int nfd = install_socket(ns);
    if (nfd < 0) { ksock_close(ns); return nfd; }
    write_sockaddr(uaddr, ulen_ptr, pip, pport);
    return nfd;
}

static int64_t sys_sendto(int fd, uint64_t ubuf, uint64_t len, int flags,
                          uint64_t uaddr, uint32_t ulen) {
    (void)flags;
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    if (len && !paging_user_ok(cur_vm(), ubuf, len, 0)) return -EFAULT_;
    uint8_t *kbuf = (uint8_t *)kmalloc(len ? len : 1);
    if (!kbuf) return -ENOMEM_;
    int r;
    if (len && copy_from_user(kbuf, cur_vm(), ubuf, len) < 0) { kfree(kbuf); return -EFAULT_; }
    if (uaddr) {
        uint32_t ip; uint16_t port;
        if (read_sockaddr(uaddr, ulen, &ip, &port) < 0) { kfree(kbuf); return -EINVAL_; }
        r = ksock_sendto(ks, kbuf, (uint32_t)len, ip, port);
    } else {
        r = ksock_send(ks, kbuf, (uint32_t)len);
    }
    kfree(kbuf);
    return r < 0 ? -EPIPE_ : r;
}

static int64_t sys_recvfrom(int fd, uint64_t ubuf, uint64_t len, int flags,
                            uint64_t uaddr, uint64_t ulen_ptr) {
    (void)flags;
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    if (len && !paging_user_ok(cur_vm(), ubuf, len, 1)) return -EFAULT_;
    uint8_t *kbuf = (uint8_t *)kmalloc(len ? len : 1);
    if (!kbuf) return -ENOMEM_;
    uint32_t ip = 0; uint16_t port = 0;
    int r = ksock_recvfrom(ks, kbuf, (uint32_t)len, &ip, &port);
    if (r > 0 && copy_to_user(cur_vm(), ubuf, kbuf, (uint32_t)r) < 0) { kfree(kbuf); return -EFAULT_; }
    kfree(kbuf);
    if (r >= 0 && uaddr) write_sockaddr(uaddr, ulen_ptr, ip, port);
    return r;
}

/* Minimal poll: report immediately-ready socket fds; one coarse retry on the
   given timeout. Enough for simple readiness checks. */
static int64_t sys_poll(uint64_t ufds, uint32_t nfds, int timeout_ms) {
    struct pollfd { int fd; int16_t events; int16_t revents; };
    if (nfds == 0) return 0;
    if (nfds > 64) return -EINVAL_;
    struct pollfd fds[64];
    if (copy_from_user(fds, cur_vm(), ufds, nfds * sizeof(struct pollfd)) < 0) return -EFAULT_;
    int waited = 0;
    for (;;) {
        int ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            void *ks = sock_get(fds[i].fd);
            if (ks) {
                int want = ((fds[i].events & 0x1) ? NET_POLLIN : 0) |
                           ((fds[i].events & 0x4) ? NET_POLLOUT : 0);
                int got = ksock_poll(ks, want);
                if (got & NET_POLLIN)  fds[i].revents |= 0x1;
                if (got & NET_POLLOUT) fds[i].revents |= 0x4;
            } else if (fds[i].fd >= 1 && fds[i].fd <= 2) {
                fds[i].revents |= (fds[i].events & 0x4);   /* console always writable */
            }
            if (fds[i].revents) ready++;
        }
        if (ready || timeout_ms == 0 || waited) {
            copy_to_user(cur_vm(), ufds, fds, nfds * sizeof(struct pollfd));
            return ready;
        }
        ksleep_ms(timeout_ms > 50 ? 50 : (uint64_t)timeout_ms);
        waited = 1;
    }
}

static uint64_t do_syscall(syscall_frame_t *f) {
    switch (f->rax) {
    case 0:   return (uint64_t)sys_read((int)f->rdi, (void *)f->rsi, f->rdx);
    case 1:   return (uint64_t)sys_write((int)f->rdi, (const void *)f->rsi, f->rdx);
    case 2:   return (uint64_t)sys_openat(AT_FDCWD, (const char *)f->rdi,
                                          (int)f->rsi, (int)f->rdx);   /* open */
    case 3:   return (uint64_t)sys_close((int)f->rdi);         /* close */
    case 4:   return (uint64_t)sys_stat((const char *)f->rdi,
                                        (struct linux_stat *)f->rsi);  /* stat */
    case 5:   return (uint64_t)sys_fstat((int)f->rdi, (struct linux_stat *)f->rsi); /* fstat */
    case 6:   return (uint64_t)sys_lstat((const char *)f->rdi,
                                        (struct linux_stat *)f->rsi);  /* lstat */
    case 8:   return (uint64_t)sys_lseek((int)f->rdi, (int64_t)f->rsi, (int)f->rdx); /* lseek */
    case 9:   return (uint64_t)sys_mmap(f->rdi, f->rsi, (int)f->rdx, (int)f->r10,
                                        (int)f->r8, f->r9);            /* mmap */
    case 10:  return (uint64_t)sys_mprotect(f->rdi, f->rsi, (int)f->rdx);  /* mprotect */
    case 11:  return (uint64_t)sys_munmap(f->rdi, f->rsi);      /* munmap */
    case 12:  return sys_brk(f->rdi);
    case 13:  return (uint64_t)sys_rt_sigaction((int)f->rdi, (const void *)f->rsi,
                                                (void *)f->rdx, f->r10);  /* rt_sigaction */
    case 14:  return (uint64_t)sys_rt_sigprocmask((int)f->rdi, (const void *)f->rsi,
                                                  (void *)f->rdx, f->r10); /* rt_sigprocmask */
    case 15:  return signals_sigreturn(f);                     /* rt_sigreturn */
    case 16:  return (uint64_t)(int64_t)-ENOTTY_;              /* ioctl: not a tty */
    case 20:  return (uint64_t)sys_writev((int)f->rdi, (const struct iovec *)f->rsi,
                                          (int)f->rdx);        /* writev */
    case 22:  return (uint64_t)sys_pipe((int *)f->rdi);        /* pipe */
    case 28:  return 0;                                        /* madvise (stub) */
    case 32:  return (uint64_t)sys_dup((int)f->rdi);           /* dup */
    case 33:  return (uint64_t)sys_dup2((int)f->rdi, (int)f->rsi);  /* dup2 */
    case 39:  return (uint64_t)task_current()->id;             /* getpid */
    case 7:   return (uint64_t)sys_poll(f->rdi, (uint32_t)f->rsi, (int)f->rdx);  /* poll */
    case 41:  return (uint64_t)sys_socket((int)f->rdi, (int)f->rsi, (int)f->rdx); /* socket */
    case 42:  return (uint64_t)sys_connect((int)f->rdi, f->rsi, (uint32_t)f->rdx); /* connect */
    case 43:  return (uint64_t)sys_accept((int)f->rdi, f->rsi, f->rdx);  /* accept */
    case 288: return (uint64_t)sys_accept((int)f->rdi, f->rsi, f->rdx);  /* accept4 */
    case 44:  return (uint64_t)sys_sendto((int)f->rdi, f->rsi, f->rdx, (int)f->r10,
                                          f->r8, (uint32_t)f->r9);       /* sendto */
    case 45:  return (uint64_t)sys_recvfrom((int)f->rdi, f->rsi, f->rdx, (int)f->r10,
                                            f->r8, f->r9);               /* recvfrom */
    case 49:  return (uint64_t)sys_bind((int)f->rdi, f->rsi, (uint32_t)f->rdx);  /* bind */
    case 50:  return (uint64_t)sys_listen((int)f->rdi, (int)f->rsi);    /* listen */
    case 48:  return 0;                                        /* shutdown (stub) */
    case 51:  return (uint64_t)sys_getsockname((int)f->rdi, f->rsi, f->rdx, 0); /* getsockname */
    case 52:  return (uint64_t)sys_getsockname((int)f->rdi, f->rsi, f->rdx, 1); /* getpeername */
    case 54:  return 0;                                        /* setsockopt (accept all) */
    case 55:  return (uint64_t)sys_getsockopt((int)f->rdi, (int)f->rsi, (int)f->rdx,
                                              f->r10, f->r8);  /* getsockopt */
    case 62:  return (uint64_t)sys_kill((int)f->rdi, (int)f->rsi);   /* kill */
    case 200: return (uint64_t)sys_kill((int)f->rdi, (int)f->rsi);   /* tkill(tid,sig) */
    case 234: return (uint64_t)sys_tgkill((int)f->rdi, (int)f->rsi, (int)f->rdx); /* tgkill */
    case 127: return (uint64_t)sys_rt_sigpending((void *)f->rdi, f->rsi);  /* rt_sigpending */
    case 131: return (uint64_t)sys_sigaltstack((const void *)f->rdi, (void *)f->rsi); /* sigaltstack */
    case 72:  return (uint64_t)sys_fcntl((int)f->rdi, (int)f->rsi, f->rdx);  /* fcntl */
    case 186: return (uint64_t)task_current()->id;             /* gettid (== pid, no threads) */
    case 79:  return (uint64_t)sys_getcwd((char *)f->rdi, f->rsi);  /* getcwd */
    case 80:  return (uint64_t)sys_chdir((const char *)f->rdi);     /* chdir */
    case 83:  return (uint64_t)sys_mkdir((const char *)f->rdi, (int)f->rsi);   /* mkdir */
    case 258: return (uint64_t)sys_mkdir((const char *)f->rsi, (int)f->rdx);   /* mkdirat */
    case 21:  return (uint64_t)sys_access((const char *)f->rdi, (int)f->rsi);   /* access */
    case 269: return (uint64_t)sys_access((const char *)f->rsi, (int)f->rdx);   /* faccessat(dirfd,path,mode) */
    case 439: return (uint64_t)sys_access((const char *)f->rsi, (int)f->rdx);   /* faccessat2 */
    case 332: return (uint64_t)sys_statx((int)f->rdi, (const char *)f->rsi, (int)f->rdx,
                                         (unsigned)f->r10, f->r8);              /* statx */
    case 280: return 0;                                        /* utimensat (no mtime store) */
    case 88:  return (uint64_t)sys_symlink((const char *)f->rdi, (const char *)f->rsi); /* symlink */
    case 89:  return (uint64_t)sys_readlink((const char *)f->rdi, (char *)f->rsi, f->rdx); /* readlink */
    case 267: return (uint64_t)sys_readlink((const char *)f->rsi, (char *)f->rdx, f->r10); /* readlinkat */
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
    case 35: {                                                 /* nanosleep */
        struct timespec { int64_t sec; int64_t nsec; } ts;
        if (copy_from_user(&ts, cur_vm(), f->rdi, sizeof ts) < 0) return (uint64_t)(int64_t)-EFAULT_;
        uint64_t ms = (uint64_t)ts.sec * 1000 + (uint64_t)ts.nsec / 1000000;
        ksleep_ms(ms);
        return 0;
    }
    case 218: return (uint64_t)task_current()->id;             /* set_tid_address -> tid */
    case 309: {                                                /* getcpu */
        unsigned cpu = (unsigned)smp_cpu_index();
        unsigned node = 0;
        if (f->rdi && copy_to_user(cur_vm(), f->rdi, &cpu, sizeof cpu) < 0)
            return (uint64_t)(int64_t)-EFAULT_;
        if (f->rsi && copy_to_user(cur_vm(), f->rsi, &node, sizeof node) < 0)
            return (uint64_t)(int64_t)-EFAULT_;
        return 0;
    }
    case 63: {                                                 /* uname */
        struct utsname { char sysname[65], nodename[65], release[65],
                              version[65], machine[65], domainname[65]; } u;
        kmemset(&u, 0, sizeof u);
        const char *vals[6] = { "VibeOS", "vibeos", "0.9", "VibeOS x86_64 SMP", "x86_64", "(none)" };
        char *flds[6] = { u.sysname, u.nodename, u.release, u.version, u.machine, u.domainname };
        for (int i = 0; i < 6; i++)
            for (int j = 0; vals[i][j] && j < 64; j++) flds[i][j] = vals[i][j];
        if (copy_to_user(cur_vm(), f->rdi, &u, sizeof u) < 0) return (uint64_t)(int64_t)-EFAULT_;
        return 0;
    }
    case 228: {                                                /* clock_gettime */
        uint64_t t = timer_ticks();                            /* 100 Hz */
        struct { int64_t sec, nsec; } ts = { (int64_t)(t / 100), (int64_t)((t % 100) * 10000000) };
        if (copy_to_user(cur_vm(), f->rsi, &ts, sizeof ts) < 0) return (uint64_t)(int64_t)-EFAULT_;
        return 0;
    }
    case 96: {                                                 /* gettimeofday */
        uint64_t t = timer_ticks();
        struct { int64_t sec, usec; } tv = { (int64_t)(t / 100), (int64_t)((t % 100) * 10000) };
        if (f->rdi && copy_to_user(cur_vm(), f->rdi, &tv, sizeof tv) < 0) return (uint64_t)(int64_t)-EFAULT_;
        return 0;
    }
    case 318: {                                                /* getrandom(buf, len, flags) */
        uint64_t ubuf = f->rdi, len = f->rsi;
        if (len && !paging_user_ok(cur_vm(), ubuf, len, 1)) return (uint64_t)(int64_t)-EFAULT_;
        uint8_t tmp[256];
        uint64_t done = 0;
        while (done < len) {
            uint32_t c = (uint32_t)((len - done) < sizeof tmp ? (len - done) : sizeof tmp);
            csprng_bytes(tmp, c);
            if (copy_to_user(cur_vm(), ubuf + done, tmp, c) < 0) return (uint64_t)(int64_t)-EFAULT_;
            done += c;
        }
        return (uint64_t)len;
    }
    case 60:                                                   /* exit */
    case 231: sys_exit((int)f->rdi);                           /* exit_group */
    default:
        kprintf("[syscall] unknown nr=%lu\n", (unsigned long)f->rax);
        return (uint64_t)-ENOSYS_;
    }
}

/* Syscall entry point (usermode.S). Run the call, record its result in the saved
   frame, then deliver any pending signal on the way back to userspace — which
   may rewrite the frame to enter a handler or terminate the task (ROADMAP §3). */
extern "C" uint64_t syscall_dispatch(syscall_frame_t *f) {
    uint64_t ret = do_syscall(f);
    f->rax = ret;
    signals_deliver_syscall(f);
    return f->rax;
}
