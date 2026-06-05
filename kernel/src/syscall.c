#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "task.h"
#include "file.h"
#include "pipe.h"
#include "fs.h"
#include "timer.h"
#include "rtc.h"
#include "tty.h"
#include "usermode.h"
#include "smp.h"
#include "signal.h"
#include "net.h"
#include "synth.h"
#include "csprng.h"
#include "config.h"
#include "fb.h"

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
#define EPERM_      1
#define ENOENT_     2
#define EINTR_      4
#define EAGAIN_     11
#define ENOTEMPTY_  39
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
#define ENODEV_     19

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
#define AT_REMOVEDIR   0x200

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

/* After changing the current address space's mappings (removing or demoting
   pages), flush the other cores' TLBs — but only if this AS is shared by sibling
   threads that may be running elsewhere. A single-threaded process (the common
   case) has ref==1 and never pays for the IPI; its own core was flushed locally. */
static inline void mm_flush_shared(task_t *t) {
    if (t->vm && __atomic_load_n(&t->vm->ref, __ATOMIC_ACQUIRE) > 1)
        tlb_shootdown_user();
}

static int64_t sys_fork(syscall_frame_t *f) {
    task_t *parent = task_current();
    if (!parent->vm) return -38;                /* kernel thread can't fork */
    vmspace_t *cvm = vmspace_fork(parent->vm);
    if (!cvm) return -12;                        /* -ENOMEM */
    mm_flush_shared(parent);                     /* sibling threads: drop stale W entries */
    task_t *child = task_fork("user", cvm, f);
    if (!child) { vmspace_destroy(cvm); return -11; }  /* -EAGAIN */
    return child->id;                            /* parent gets child's pid */
}

/* clone(2) flag bits we recognize (musl's pthread_create set, ROADMAP). */
#define CLONE_VM             0x00000100
#define CLONE_FILES          0x00000400
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* clone(flags, stack, ptid, ctid, tls) — enough for musl pthreads: a CLONE_VM
   thread sharing the address space + fd table + thread group, with its own
   stack + TLS, plus the parent/child TID and CHILD_CLEARTID plumbing
   pthread_join relies on. Without CLONE_VM it degrades to a fork. */
static int64_t sys_clone(syscall_frame_t *f) {
    task_t *parent = task_current();
    if (!parent->vm) return -38;
    uint64_t flags = f->rdi, ustack = f->rsi;
    uint64_t ptid = f->rdx, ctid = f->r10, tls = f->r8;

    vmspace_t *vm;
    if (flags & CLONE_VM) { vm = parent->vm; vmspace_ref(vm); }   /* shared AS */
    else { vm = vmspace_fork(parent->vm); if (!vm) return -12; }  /* fork-like */

    int is_thread = (flags & CLONE_THREAD) != 0;
    task_t *c = task_clone(is_thread ? "thread" : "user", vm, f, ustack,
                           (flags & CLONE_SETTLS) ? tls : 0,
                           (flags & CLONE_FILES) != 0, is_thread);
    if (!c) { vmspace_destroy(vm); return -11; }

    if (flags & CLONE_PARENT_SETTID)
        copy_to_user(cur_vm(), ptid, &c->id, sizeof(int));
    if (flags & CLONE_CHILD_SETTID)             /* shared AS -> visible to child */
        copy_to_user(cur_vm(), ctid, &c->id, sizeof(int));
    if (flags & CLONE_CHILD_CLEARTID)
        c->clear_child_tid = ctid;              /* cleared + futex-woken at exit */
    return c->id;
}

/* A futex word's key: the process's PML4 phys (shared by its threads) mixed with
   the user address. Threads in one process collide on the same word; distinct
   processes don't (a collision would only cause a harmless spurious wake). */
static uint64_t futex_key(uint64_t uaddr) {
    return cur_vm()->pml4_phys ^ uaddr;
}

/* futex(uaddr, op, val, timeout, uaddr2, val3) — FUTEX_WAIT / FUTEX_WAKE (and
   their _BITSET aliases). Private/realtime flag bits are ignored. */
static int64_t sys_futex(uint64_t uaddr, int op, int val) {
    enum { FUTEX_WAIT=0, FUTEX_WAKE=1, FUTEX_WAIT_BITSET=9, FUTEX_WAKE_BITSET=10 };
    int cmd = op & 0x7f;                        /* drop FUTEX_PRIVATE/CLOCK bits */
    if (!paging_user_ok(cur_vm(), uaddr, sizeof(int), 0)) return -EFAULT_;
    uint64_t key = futex_key(uaddr);

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        sched_lock();
        int cur = 0;
        if (copy_from_user(&cur, cur_vm(), uaddr, sizeof cur) < 0) { sched_unlock(); return -EFAULT_; }
        if (cur != val) { sched_unlock(); return -11; }   /* -EAGAIN: value changed */
        futex_sleep_on(key);                    /* sleeps under the lock; rechecks in musl */
        sched_unlock();
        return 0;
    }
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        sched_lock();
        int woke = futex_wake_key(key, val);
        sched_unlock();
        return woke;
    }
    return -38;                                  /* -ENOSYS for other ops */
}

/* Thread exit (CLONE_CHILD_CLEARTID): zero the tid word and futex-wake any
   joiner, then release this thread's resources (no zombie). */
__attribute__((noreturn))
static void sys_exit_thread(void) {
    task_t *t = task_current();
    if (t->clear_child_tid) {
        int zero = 0;
        copy_to_user(cur_vm(), t->clear_child_tid, &zero, sizeof zero);
        uint64_t key = futex_key(t->clear_child_tid);
        sched_lock(); futex_wake_key(key, 1); sched_unlock();
    }
    task_exit_thread();
    __builtin_unreachable();
}

/* ---- per-process fd helpers ---- */

/* Resolve a user fd to its open-file object, or NULL if it is out of range,
   closed, or one of the implicit console fds (0/1/2). A bare read for transient
   in-syscall use; concurrently closing an fd you are still using is an app-level
   race (POSIX-unspecified), and the file pool is static so it cannot fault. */
static file_t *fd_get(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD) return nullptr;
    return task_current()->files->fd[fd];
}

/* Like fd_get but takes a reference under the table lock, so the returned file
   cannot be closed out from under the caller before it is consumed (dup family,
   which persists the new reference). The caller balances it with file_unref. */
static file_t *fd_get_ref(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD) return nullptr;
    fdtable_t *ft = task_current()->files;
    spin_lock(&ft->lock);
    file_t *f = ft->fd[fd];
    if (f) file_ref(f);
    spin_unlock(&ft->lock);
    return f;
}

/* Install `f` at the lowest free descriptor >= 3 (0/1/2 are the console).
   Returns the fd, or -EMFILE if the table is full. The scan-and-claim is under
   the table lock so two cores can't hand out the same slot. */
static int fd_install(file_t *f) {
    fdtable_t *ft = task_current()->files;
    spin_lock(&ft->lock);
    for (int i = 3; i < VFS_MAX_FD; i++)
        if (!ft->fd[i]) {
            ft->fd[i] = f; ft->cloexec[i] = 0;
            spin_unlock(&ft->lock);
            return i;
        }
    spin_unlock(&ft->lock);
    return -EMFILE_;
}

/* Detach the file at `fd` from the current task's table (under the lock) and
   return it for the caller to unref, or NULL if the slot was empty. */
static file_t *fd_take(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD) return nullptr;
    fdtable_t *ft = task_current()->files;
    spin_lock(&ft->lock);
    file_t *f = ft->fd[fd];
    ft->fd[fd] = nullptr;
    ft->cloexec[fd] = 0;
    spin_unlock(&ft->lock);
    return f;
}

/* ---- basic I/O / exit ---- */

/* eventfd/timerfd read-write helpers live with the poll machinery below. */
static int64_t eventfd_read(file_t *f, uint64_t ubuf, uint64_t n);
static int64_t eventfd_write(file_t *f, uint64_t ubuf, uint64_t n);
static int64_t timerfd_read(file_t *f, uint64_t ubuf, uint64_t n);

static int64_t sys_write(int fd, const void *buf, uint64_t n) {
    if (n && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)buf, n, 0))
        return -EFAULT_;                        /* bad user buffer: no kernel fault */
    file_t *f = fd_get(fd);
    if (!f) {                                   /* 0/1/2 default to the console... */
        if (fd == 1 || fd == 2) return tty_write((const char *)buf, (uint32_t)n);
        return -EBADF_;                         /* ...unless redirected onto a file */
    }
    if (f->kind == FD_PIPE_WR) return pipe_write(f->pipe, buf, (uint32_t)n, f->flags);
    if (f->kind == FD_PIPE_RD) return -EBADF_;  /* write to read end */
    if (f->kind == FD_SOCKET) {
        int r = ksock_send(f->sock, buf, (uint32_t)n, (f->flags & 04000) ? 1 : 0);
        if (r == -11) return -11;               /* -EAGAIN: send buffer full (non-blocking) */
        return r < 0 ? -EPIPE_ : r;
    }
    if (f->kind == FD_EVENT) return eventfd_write(f, (uint64_t)(uintptr_t)buf, n);
    if (f->kind == FD_TIMER) return -EINVAL_;   /* timerfd is read-only */
    if (f->kind == FD_DEV || f->kind == FD_PROC) return synth_write(f, buf, (uint32_t)n);
    if (f->kind != FD_FILE) return -EISDIR_;
    /* Snapshot the offset under the file lock (not held across the blocking fs
       call), so a fork/dup-shared description's `off` can't be lost-updated by a
       writer on another core. */
    uint64_t off;
    spin_lock(&f->off_lock);
    off = f->off;
    spin_unlock(&f->off_lock);
    if (f->flags & O_APPEND) {                  /* append: write at EOF */
        fs_stat_t st;
        if (fs_istat(f->ino, &st) == FS_OK) off = st.size;
    }
    int r = fs_pwrite(f->ino, off, buf, (uint32_t)n);
    if (r < 0) return fs_to_errno(r);
    spin_lock(&f->off_lock);
    f->off = off + (uint64_t)r;
    spin_unlock(&f->off_lock);
    return r;
}

static int64_t sys_read(int fd, void *buf, uint64_t n) {
    if (n && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)buf, n, 1))
        return -EFAULT_;                        /* writable check + COW break */
    file_t *f = fd_get(fd);
    if (!f) {                                   /* 0 defaults to the console TTY */
        if (fd == 0) return tty_read((char *)buf, (uint32_t)n);
        return -EBADF_;
    }
    if (f->kind == FD_PIPE_RD) return pipe_read(f->pipe, buf, (uint32_t)n, f->flags);
    if (f->kind == FD_PIPE_WR) return -EBADF_;  /* read from write end */
    if (f->kind == FD_SOCKET) return ksock_recv(f->sock, buf, (uint32_t)n, (f->flags & 04000) ? 1 : 0);
    if (f->kind == FD_EVENT) return eventfd_read(f, (uint64_t)(uintptr_t)buf, n);
    if (f->kind == FD_TIMER) return timerfd_read(f, (uint64_t)(uintptr_t)buf, n);
    if (f->kind == FD_DEV || f->kind == FD_PROC) return synth_read(f, buf, (uint32_t)n);
    if (f->kind == FD_DEVDIR) return -EISDIR_;
    if (f->kind != FD_FILE) return -EISDIR_;
    uint64_t off;
    spin_lock(&f->off_lock);                    /* see sys_write: snapshot, then advance */
    off = f->off;
    spin_unlock(&f->off_lock);
    int r = fs_pread(f->ino, off, buf, (uint32_t)n);
    if (r < 0) return fs_to_errno(r);
    spin_lock(&f->off_lock);
    f->off = off + (uint64_t)r;
    spin_unlock(&f->off_lock);
    return r;
}

/* ioctl(2): dispatch on the fd's backing object. The implicit console fds (0/1/2)
   and /dev/tty carry the terminal discipline (termios/winsize/job control); a
   pipe/socket/regular file is -ENOTTY (which is how isatty(3) tells them apart). */
static int64_t sys_ioctl(int fd, unsigned cmd, uint64_t arg) {
    file_t *f = fd_get(fd);
    if (!f) {
        if (fd == 0 || fd == 1 || fd == 2) return tty_ioctl(cmd, arg);
        return -EBADF_;
    }
    if (f->kind == FD_DEV) return synth_ioctl(f, cmd, arg);
    return -ENOTTY_;
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
    /* Close descriptors marked FD_CLOEXEC (ROADMAP §4). Detach each under the
       table lock, then unref outside it (unref may take other locks). */
    for (int i = 3; i < VFS_MAX_FD; i++) {
        spin_lock(&t->files->lock);
        file_t *cf = (t->files->cloexec[i] && t->files->fd[i]) ? t->files->fd[i] : nullptr;
        if (cf) t->files->fd[i] = nullptr;
        t->files->cloexec[i] = 0;
        spin_unlock(&t->files->lock);
        if (cf) file_unref(cf);         /* closes the socket at the last ref */
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

static int64_t sys_readv(int fd, const struct iovec *iov, int cnt) {
    if (cnt < 0) return -EINVAL_;
    if (cnt && !paging_user_ok(cur_vm(), (uint64_t)(uintptr_t)iov,
                               (uint64_t)cnt * sizeof(struct iovec), 0))
        return -EFAULT_;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t r = sys_read(fd, (void *)iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break;
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
    uint64_t base = ((flags & MAP_FIXED) && addr)
        ? PAGE_ALIGN_DOWN(addr)
        : __atomic_fetch_add(&t->vm->mmap_next, mlen, __ATOMIC_RELAXED);
    if (anon && prot == 0) return (int64_t)base;     /* PROT_NONE: reserve VA only */

    file_t *fl = nullptr;
    if (!anon) {
        fl = fd_get(fd);
        if (!fl) return -EBADF_;
        /* Device mmap: /dev/fb0 maps the linear framebuffer's physical pages
           straight into the user address space (GUI phase 2). MAP_SHARED — the
           server writes pixels the scanout reads. */
        if (fl->kind == FD_DEV && fl->dev == SYNTH_DEV_FB0) {
            uint64_t fbp = fb_phys_base();
            uint64_t fbsz = PAGE_ALIGN_UP(fb_size_bytes());
            if (!fbp || !fbsz) return -ENODEV_;
            if (mlen > fbsz) mlen = fbsz;
            for (uint64_t o = 0; o < mlen; o += PAGE_SIZE)
                vmspace_map(t->vm, base + o, fbp + o, 1, PTE_P | PTE_W | PTE_U);
            return (int64_t)base;
        }
        if (fl->kind != FD_FILE) return -EBADF_;
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
    mm_flush_shared(t);                          /* other cores: drop the dead mappings */
    return 0;
}

/* mremap (ROADMAP §4): resize an anonymous mapping. musl's realloc() of large
   (mmap-backed) chunks calls this — e.g. the GUI window manager growing its
   per-window receive buffer on a resize drag. Our mmap is a bump-arena with no
   per-region size table, so we cannot safely extend in place (the VA just past a
   region may be a PROT_NONE guard musl reserved). We therefore honour the model
   musl always asks for — MREMAP_MAYMOVE — by allocating a fresh region above
   mmap_next, copying the live bytes, and unmapping the old one. Shrink frees the
   tail in place. */
#define MREMAP_MAYMOVE 1
static int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len,
                          int flags, uint64_t new_addr) {
    (void)new_addr;
    if ((old_addr & PAGE_MASK) || new_len == 0) return -EINVAL_;
    task_t *t = task_current();
    uint64_t old_pg = PAGE_ALIGN_UP(old_len);
    uint64_t new_pg = PAGE_ALIGN_UP(new_len);

    if (new_pg == old_pg) return (int64_t)old_addr;        /* same page count */

    if (new_pg < old_pg) {                                  /* shrink: free tail */
        sys_munmap(old_addr + new_pg, old_pg - new_pg);
        return (int64_t)old_addr;
    }

    /* grow: relocate (we can't trust the VA above the region to be free). */
    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM_;
    uint64_t base = __atomic_fetch_add(&t->vm->mmap_next, new_pg, __ATOMIC_RELAXED);
    for (uint64_t va = base; va < base + new_pg; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_page();                     /* zeroed by the PMM */
        if (!pa) return -ENOMEM_;
        vmspace_map(t->vm, va, pa, 1, PTE_P | PTE_W | PTE_U);
    }
    /* same address space is live, so the old + new VAs are both reachable */
    kmemcpy((void *)(uintptr_t)base, (const void *)(uintptr_t)old_addr,
            old_len < new_len ? old_len : new_len);
    sys_munmap(old_addr, old_pg);
    return (int64_t)base;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot) {
    len  = PAGE_ALIGN_UP(len);
    addr = PAGE_ALIGN_DOWN(addr);
    task_t *t = task_current();
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        uint64_t pa;
        if (!vmspace_query(t->vm, va, &pa)) {
            /* Unmapped: musl reserves regions with mmap(PROT_NONE) then mprotect()s
               the usable part RW (e.g. thread stacks). Lazily back it with a fresh
               zero page when it becomes accessible; leave PROT_NONE reserved. */
            if (prot == 0) continue;
            pa = pmm_alloc_page();
            if (!pa) return -ENOMEM_;
            uint64_t fl = PTE_P | PTE_U | ((prot & PROT_WRITE) ? PTE_W : 0);
            vmspace_map(t->vm, va, pa, 1, fl);
            continue;
        }
        if (prot & PROT_WRITE) {
            paging_cow_fault(t->vm, va);            /* privatise if COW-shared */
            if (!vmspace_query(t->vm, va, &pa)) continue;  /* pa may have moved */
            vmspace_map(t->vm, va, pa, 1, PTE_P | PTE_U | PTE_W);
        } else {
            vmspace_map(t->vm, va, pa, 1, PTE_P | PTE_U);   /* may drop write */
        }
    }
    mm_flush_shared(t);          /* other cores: honor any write-permission downgrade */
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
    uint32_t perm = s->mode & 07777u;            /* stored permission bits */
    if (perm == 0)                               /* legacy inode (pre-mode): synthesize */
        perm = (s->type == FT_DIR) ? 0755u : (s->type == FT_SYMLINK) ? 0777u : 0644u;
    st->st_mode  = ((s->type == FT_DIR)     ? S_IFDIR
                 :  (s->type == FT_SYMLINK) ? S_IFLNK
                                            : S_IFREG) | perm;
    st->st_size  = (int64_t)s->size;
    st->st_blksize = FS_BLOCK_SIZE;
    st->st_blocks  = (int64_t)((s->size + 511) / 512);
    uint64_t ep = rtc_boot_epoch();  /* ticks are boot-relative; anchor to wall clock */
    st->st_mtime = (int64_t)(ep + s->mtime / 100);
    st->st_ctime = (int64_t)(ep + s->ctime / 100);
    st->st_atime = (int64_t)(ep + s->mtime / 100);
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
        if (flags & O_CLOEXEC) task_current()->files->cloexec[sfd] = 1;
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
    if (flags & O_CLOEXEC) task_current()->files->cloexec[fd] = 1;
    return fd;
}

static int64_t sys_close(int fd) {
    file_t *f = fd_take(fd);                 /* detach under the table lock */
    if (!f) return (fd >= 0 && fd <= 2) ? 0 : -EBADF_;   /* bare console: no-op */
    file_unref(f);                           /* closes pipe/socket at the last ref */
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

/* sysconfig(op, a, buf, len) — VibeOS-private access to the /config service
   (ROADMAP: config service). A high, non-Linux number so musl never calls it;
   /bin/sysconf drives it. ops: 0=RELOAD (re-read /config), 1=GET (value of key
   `a`), 2=COUNT, 3=ENTRY (i-th "key=value" where i=`a`). */
static int64_t sys_sysconfig(int op, uint64_t a, uint64_t ubuf, uint64_t len) {
    enum { CFG_RELOAD = 0, CFG_GET = 1, CFG_COUNT = 2, CFG_ENTRY = 3 };
    char out[256];
    switch (op) {
    case CFG_RELOAD: return config_reload();
    case CFG_COUNT:  return config_count();
    case CFG_GET: {
        char key[64]; copy_user_str(key, (const char *)a, sizeof key);
        const char *v = config_get(key);
        if (!v) return -ENOENT_;
        unsigned n = 0; for (; v[n] && n < sizeof out - 1; n++) out[n] = v[n]; out[n] = '\0';
        uint64_t c = n < len ? n : len;
        if (c && copy_to_user(cur_vm(), ubuf, out, c) < 0) return -EFAULT_;
        return (int64_t)n;
    }
    case CFG_ENTRY: {
        const char *k, *v;
        if (!config_entry((int)a, &k, &v)) return -ENOENT_;
        unsigned n = 0;
        for (unsigned i = 0; k[i] && n < sizeof out - 2; i++) out[n++] = k[i];
        out[n++] = '=';
        for (unsigned i = 0; v[i] && n < sizeof out - 1; i++) out[n++] = v[i];
        out[n] = '\0';
        uint64_t c = n < len ? n : len;
        if (c && copy_to_user(cur_vm(), ubuf, out, c) < 0) return -EFAULT_;
        return (int64_t)n;
    }
    default: return -EINVAL_;
    }
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
    file_t *f = fd_get(fd);
    if (!f) {                                   /* bare console (0/1/2) */
        if (fd >= 0 && fd <= 2) { fill_stat_console(out); return 0; }
        return -EBADF_;
    }
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

/* unlink(path): remove a name for a regular file/symlink (never a directory). */
static int64_t sys_unlink(const char *upath) {
    char path[256];
    resolve_path(path, sizeof path, upath);
    if (synth_classify(path, nullptr) != SYNTH_NONE) return -EACCES_;   /* /dev,/proc read-only */
    int ino = fs_lresolve(path);                     /* operate on the link itself */
    if (ino < 0) return fs_to_errno(ino);
    fs_stat_t s;
    if (fs_istat((uint32_t)ino, &s) == FS_OK && s.type == FT_DIR) return -EISDIR_;
    int r = fs_unlink(path);
    return r == FS_OK ? 0 : fs_to_errno(r);
}

/* rmdir(path): remove an empty directory. */
static int64_t sys_rmdir(const char *upath) {
    char path[256];
    resolve_path(path, sizeof path, upath);
    if (synth_classify(path, nullptr) != SYNTH_NONE) return -EACCES_;
    int ino = fs_lresolve(path);
    if (ino < 0) return fs_to_errno(ino);
    fs_stat_t s;
    if (fs_istat((uint32_t)ino, &s) != FS_OK) return -ENOENT_;
    if (s.type != FT_DIR) return -ENOTDIR_;
    int r = fs_unlink(path);                          /* enforces emptiness -> ENOTEMPTY */
    return r == FS_OK ? 0 : fs_to_errno(r);
}

/* unlinkat(dirfd, path, flags): unlink, or rmdir when AT_REMOVEDIR is set. */
static int64_t sys_unlinkat(int dirfd, const char *upath, int flags) {
    (void)dirfd;                                      /* AT_FDCWD / cwd-relative only */
    return (flags & AT_REMOVEDIR) ? sys_rmdir(upath) : sys_unlink(upath);
}

/* rename(old, new) / renameat: move a file or directory. */
static int64_t sys_rename(const char *uold, const char *unew) {
    char oldp[256], newp[256];
    resolve_path(oldp, sizeof oldp, uold);
    resolve_path(newp, sizeof newp, unew);
    if (synth_classify(oldp, nullptr) != SYNTH_NONE ||
        synth_classify(newp, nullptr) != SYNTH_NONE) return -EACCES_;
    int r = fs_rename(oldp, newp);
    return r == FS_OK ? 0 : fs_to_errno(r);
}

/* ftruncate(fd, len): resize an open, writable regular file. */
static int64_t sys_ftruncate(int fd, int64_t len) {
    if (len < 0) return -EINVAL_;
    file_t *f = fd_get(fd);
    if (!f) return (fd >= 0 && fd <= 2) ? -EINVAL_ : -EBADF_;
    if (f->kind != FD_FILE) return -EINVAL_;
    if ((f->flags & 3) == 0) return -EINVAL_;         /* O_RDONLY: not writable */
    int r = fs_truncate_to(f->ino, (uint64_t)len);
    return r == FS_OK ? 0 : fs_to_errno(r);
}

/* truncate(path, len): resize a regular file by path. */
static int64_t sys_truncate(const char *upath, int64_t len) {
    if (len < 0) return -EINVAL_;
    char path[256];
    resolve_path(path, sizeof path, upath);
    if (synth_classify(path, nullptr) != SYNTH_NONE) return -EACCES_;
    int ino = fs_resolve(path);
    if (ino < 0) return fs_to_errno(ino);
    int r = fs_truncate_to((uint32_t)ino, (uint64_t)len);
    return r == FS_OK ? 0 : fs_to_errno(r);
}

/* chmod(path, mode) / fchmodat(dirfd, path, mode, flag) / fchmod(fd, mode):
   record permission bits. VibeOS has no uid model, so this isn't an access-
   control gate; it makes mode bits real so stat reflects them and shells'
   executability checks pass for binaries. */
static int64_t sys_chmod(const char *upath, uint32_t mode) {
    char path[256];
    resolve_path(path, sizeof path, upath);
    if (synth_classify(path, nullptr) != SYNTH_NONE) return -EACCES_;
    int ino = fs_resolve(path);
    if (ino < 0) return fs_to_errno(ino);
    int r = fs_chmod((uint32_t)ino, mode);
    return r == FS_OK ? 0 : fs_to_errno(r);
}

static int64_t sys_fchmod(int fd, uint32_t mode) {
    file_t *f = fd_get(fd);
    if (!f) return -EBADF_;
    if (f->kind != FD_FILE) return -EINVAL_;     /* only on-disk inodes carry a mode */
    int r = fs_chmod(f->ino, mode);
    return r == FS_OK ? 0 : fs_to_errno(r);
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

    /* The synthetic top-level mounts /dev and /proc aren't real dirents, so emit
       them after the real entries when listing the root — otherwise `ls /` would
       hide them. f->off >= SYNTH_OFF marks the synthetic phase (past any real
       directory byte offset). */
    const uint64_t SYNTH_OFF = 1ULL << 62;
    static const char *const ROOT_SYNTH[] = { "dev", "proc" };
    int is_root = ((uint32_t)f->ino == (uint32_t)fs_resolve("/"));

    uint8_t *out = (uint8_t *)ubuf;
    uint64_t used = 0;
    for (;;) {
        const char *name; uint8_t dtype; uint64_t ino_no, next_off;
        uint64_t saved = f->off;
        int synth = 0;

        if (f->off >= SYNTH_OFF) {              /* synthetic phase (root only) */
            uint64_t si = f->off - SYNTH_OFF;
            if (!is_root || si >= 2) break;
            name = ROOT_SYNTH[si]; dtype = DT_DIR; ino_no = SYNTH_OFF + si;
            next_off = f->off + 1; synth = 1;
        } else {
            fs_dirent_t de;
            int r = fs_dirent_at(f->ino, &f->off, &de);  /* advances f->off */
            if (r < 0) return fs_to_errno(r);
            if (r == 0) {                       /* end of real entries */
                if (is_root) { f->off = SYNTH_OFF; continue; }
                break;
            }
            name = de.name; ino_no = de.inode; next_off = f->off;
            dtype = (de.type == FT_DIR) ? DT_DIR : DT_REG;
        }

        unsigned namelen = (unsigned)kstrlen(name);
        unsigned reclen = (sizeof(struct linux_dirent64) + namelen + 1 + 7) & ~7u;
        if (used + reclen > count) {            /* doesn't fit: rewind, stop */
            f->off = saved;
            if (used == 0) return -EINVAL_;     /* buffer too small for one entry */
            break;
        }
        struct linux_dirent64 *d = (struct linux_dirent64 *)(out + used);
        d->d_ino = ino_no;
        d->d_off = (int64_t)next_off;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = dtype;
        kmemcpy(d->d_name, name, namelen + 1);
        used += reclen;
        if (synth) f->off = next_off;          /* real entries: fs_dirent_at already advanced */
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
        file_t *taken = fd_take(rfd);            /* undo the read-end install */
        if (taken) file_unref(taken);
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
    file_t *f = fd_get_ref(oldfd);            /* ref taken under the table lock */
    if (!f) return (oldfd >= 0 && oldfd <= 2) ? oldfd : -EBADF_;  /* bare console: identity */
    int fd = fd_install(f);
    if (fd < 0) { file_unref(f); return fd; }
    return fd;
}

/* dup oldfd onto newfd exactly, closing whatever newfd held. */
static int64_t sys_dup2(int oldfd, int newfd) {
    if (newfd < 0 || newfd >= VFS_MAX_FD) return -EBADF_;
    file_t *f = fd_get_ref(oldfd);                    /* ref taken under the lock */
    if (!f) {                                         /* oldfd is the bare console */
        if ((oldfd == 0 || oldfd == 1 || oldfd == 2) && oldfd == newfd) return newfd;
        return -EBADF_;
    }
    if (oldfd == newfd) { file_unref(f); return newfd; }   /* drop the extra ref */
    fdtable_t *ft = task_current()->files;
    spin_lock(&ft->lock);
    file_t *victim = ft->fd[newfd];                   /* whatever newfd held */
    ft->fd[newfd] = f;                                /* f already carries our ref */
    ft->cloexec[newfd] = 0;                           /* dup2 clears CLOEXEC (POSIX) */
    spin_unlock(&ft->lock);
    if (victim) file_unref(victim);                   /* close the displaced fd */
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
        if (nfd >= 0) t->files->cloexec[nfd] = 1;
        return nfd;
    }
    case F_GETFD: return (fd >= 0 && fd < VFS_MAX_FD && t->files->cloexec[fd]) ? FD_CLOEXEC_BIT : 0;
    case F_SETFD:
        if (fd >= 0 && fd < VFS_MAX_FD) t->files->cloexec[fd] = (arg & FD_CLOEXEC_BIT) ? 1 : 0;
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

/* options: WNOHANG(1) | WUNTRACED(2) | WCONTINUED(8) — passed through to task_wait,
   which reports WIFSTOPPED/WIFCONTINUED job-control events as well as exits. */
static int64_t sys_wait4(int pid, int *ustatus, int options, void *rusage) {
    (void)rusage;
    int code = 0;
    /* pid > 0: that child; pid <= 0: any child (process-group wait unsupported). */
    int got = task_wait(&code, options, pid > 0 ? pid : 0);
    if (got <= 0) return got;                    /* -ECHILD, or 0 = none ready (WNOHANG) */
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
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    if (len && !paging_user_ok(cur_vm(), ubuf, len, 0)) return -EFAULT_;
    uint8_t *kbuf = (uint8_t *)kmalloc(len ? len : 1);
    if (!kbuf) return -ENOMEM_;
    file_t *ff = fd_get(fd);
    int nb = (flags & 0x40) || (ff && (ff->flags & 04000)) ? 1 : 0;   /* MSG_DONTWAIT | O_NONBLOCK */
    int r;
    if (len && copy_from_user(kbuf, cur_vm(), ubuf, len) < 0) { kfree(kbuf); return -EFAULT_; }
    if (uaddr) {
        uint32_t ip; uint16_t port;
        if (read_sockaddr(uaddr, ulen, &ip, &port) < 0) { kfree(kbuf); return -EINVAL_; }
        r = ksock_sendto(ks, kbuf, (uint32_t)len, ip, port, nb);
    } else {
        r = ksock_send(ks, kbuf, (uint32_t)len, nb);
    }
    kfree(kbuf);
    if (r == -11) return -11;                    /* -EAGAIN */
    return r < 0 ? -EPIPE_ : r;
}

static int64_t sys_recvfrom(int fd, uint64_t ubuf, uint64_t len, int flags,
                            uint64_t uaddr, uint64_t ulen_ptr) {
    void *ks = sock_get(fd); if (!ks) return -EBADF_;
    if (len && !paging_user_ok(cur_vm(), ubuf, len, 1)) return -EFAULT_;
    uint8_t *kbuf = (uint8_t *)kmalloc(len ? len : 1);
    if (!kbuf) return -ENOMEM_;
    file_t *ff = fd_get(fd);
    int nb = (flags & 0x40) || (ff && (ff->flags & 04000)) ? 1 : 0;   /* MSG_DONTWAIT | O_NONBLOCK */
    uint32_t ip = 0; uint16_t port = 0;
    int r = ksock_recvfrom(ks, kbuf, (uint32_t)len, &ip, &port, nb);
    if (r > 0 && copy_to_user(cur_vm(), ubuf, kbuf, (uint32_t)r) < 0) { kfree(kbuf); return -EFAULT_; }
    kfree(kbuf);
    if (r >= 0 && uaddr) write_sockaddr(uaddr, ulen_ptr, ip, port);
    return r;
}

/* poll(2)/select(2) revents bits. */
#define POLLIN_   0x01
#define POLLOUT_  0x04
#define POLLERR_  0x08
#define POLLHUP_  0x10
#define POLLNVAL_ 0x20

/* eventfd/timerfd flags. */
#define EFD_SEMAPHORE 1
#define O_NONBLOCK_   04000

/* Non-destructive readiness of a single fd, as raw POLL* bits (ignores which
   events the caller wants; the poll/select layer masks). The one place that
   knows how every fd kind signals readiness. */
static int fd_ready(int fd) {
    file_t *f = fd_get(fd);
    if (!f) {                                          /* bare console fds */
        if (fd == 0) return tty_readable() ? POLLIN_ : 0;
        if (fd == 1 || fd == 2) return POLLOUT_;
        return POLLNVAL_;
    }
    int r = 0;
    switch (f->kind) {
    case FD_SOCKET: {
        int got = ksock_poll(f->sock, NET_POLLIN | NET_POLLOUT);
        if (got & NET_POLLIN)  r |= POLLIN_;
        if (got & NET_POLLOUT) r |= POLLOUT_;
        break;
    }
    case FD_PIPE_RD: r = pipe_poll(f->pipe, 0); break;
    case FD_PIPE_WR: r = pipe_poll(f->pipe, 1); break;
    case FD_EVENT:
        spin_lock(&f->off_lock);
        if (f->aux1 > 0)                       r |= POLLIN_;
        if (f->aux1 < 0xFFFFFFFFFFFFFFFEULL)   r |= POLLOUT_;
        spin_unlock(&f->off_lock);
        break;
    case FD_TIMER:
        spin_lock(&f->off_lock);
        if (f->aux1 != 0 && timer_ticks() >= f->aux1) r |= POLLIN_;
        spin_unlock(&f->off_lock);
        break;
    default:                                           /* files/dirs/synthetic: always ready */
        r = POLLIN_ | POLLOUT_;
        break;
    }
    return r;
}

/* Sleep one polling quantum, capped by the deadline. Returns 0 normally, -EINTR_
   if a signal is pending. timeout_ms < 0 means block indefinitely. */
static int poll_wait_quantum(int64_t timeout_ms, uint64_t start_tick) {
    if (signals_pending_current()) return -EINTR_;
    uint32_t hz = timer_hz() ? timer_hz() : 100;
    if (timeout_ms >= 0) {
        uint64_t elapsed_ms = (timer_ticks() - start_tick) * 1000 / hz;
        if (elapsed_ms >= (uint64_t)timeout_ms) return 1;       /* deadline reached */
        uint64_t left = (uint64_t)timeout_ms - elapsed_ms;
        ksleep_ms(left > 50 ? 50 : left);
    } else {
        ksleep_ms(50);
    }
    return 0;
}

/* rt_sigsuspend(mask, sigsetsize): atomically install `mask` as the blocked set
   and block until a deliverable signal arrives, then return -EINTR (the handler
   runs on the way back to userspace). Interactive shells use this to wait for
   SIGCHLD without busy-waiting. Simplification: the pre-suspend mask is not
   auto-restored after the handler (callers like mksh re-set it themselves); this
   matches the codebase's other sigmask approximations. */
static int64_t sys_rt_sigsuspend(const void *umask, uint64_t sigsetsize) {
    if (sigsetsize != sizeof(uint64_t)) return -EINVAL_;
    uint64_t newmask;
    if (copy_from_user(&newmask, cur_vm(), (uint64_t)(uintptr_t)umask, sizeof newmask) < 0)
        return -EFAULT_;
    signals_set_blocked_current(newmask);
    uint64_t start = timer_ticks();
    while (poll_wait_quantum(-1, start) == 0)   /* sleep in quanta until a signal is pending */
        ;
    return -EINTR_;
}

/* getppid: parent's pid, or 1 (init) for an orphan. */
static int64_t sys_getppid(void) {
    task_t *t = task_current();
    return (t && t->parent) ? task_tgid(t->parent) : 1;
}

/* flock(fd, op): advisory whole-file locking. VibeOS is single-user with no
   contending lockers, so accept it as a no-op (shells lock their history file
   with it). Just validate the fd. */
static int64_t sys_flock(int fd, int op) {
    (void)op;
    return fd_get(fd) ? 0 : -EBADF_;
}

/* umask(mask): set the process file-creation mask, return the previous value.
   Stored process-wide (a simplification; not yet applied at file creation). */
static int g_umask = 022;
static int64_t sys_umask(int mask) {
    int old = g_umask;
    g_umask = mask & 0777;
    return old;
}

/* getrusage(who, usage): no per-process accounting yet — return a zeroed struct
   so callers (e.g. mksh's job timing) succeed instead of erroring. */
static int64_t sys_getrusage(int who, uint64_t ubuf) {
    (void)who;
    if (ubuf) {
        char zero[144];                          /* sizeof(struct rusage) on x86-64 */
        kmemset(zero, 0, sizeof zero);
        if (copy_to_user(cur_vm(), ubuf, zero, sizeof zero) < 0) return -EFAULT_;
    }
    return 0;
}

static int64_t sys_poll(uint64_t ufds, uint32_t nfds, int64_t timeout_ms) {
    struct pollfd { int fd; int16_t events; int16_t revents; };
    if (nfds == 0) { if (timeout_ms > 0) ksleep_ms((uint64_t)timeout_ms); return 0; }
    if (nfds > 128) return -EINVAL_;
    struct pollfd fds[128];
    if (copy_from_user(fds, cur_vm(), ufds, nfds * sizeof(struct pollfd)) < 0) return -EFAULT_;
    uint64_t start = timer_ticks();
    for (;;) {
        int ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            int rdy = fd_ready(fds[i].fd);
            int rev = rdy & (fds[i].events | POLLERR_ | POLLHUP_ | POLLNVAL_);
            fds[i].revents = (int16_t)rev;
            if (rev) ready++;
        }
        if (ready || timeout_ms == 0) {
            if (copy_to_user(cur_vm(), ufds, fds, nfds * sizeof(struct pollfd)) < 0) return -EFAULT_;
            return ready;
        }
        int w = poll_wait_quantum(timeout_ms, start);
        if (w < 0) return w;
        if (w > 0) {
            if (copy_to_user(cur_vm(), ufds, fds, nfds * sizeof(struct pollfd)) < 0) return -EFAULT_;
            return 0;
        }
    }
}

/* ppoll(fds, nfds, timespec*, sigmask, sigsetsize). The sigmask is ignored (we
   don't atomically swap the blocked set for the wait); timeout comes from the
   timespec (NULL = block indefinitely). */
static int64_t sys_ppoll(uint64_t ufds, uint32_t nfds, uint64_t uts) {
    int64_t timeout_ms = -1;
    if (uts) {
        struct { int64_t sec, nsec; } ts;
        if (copy_from_user(&ts, cur_vm(), uts, sizeof ts) < 0) return -EFAULT_;
        timeout_ms = ts.sec * 1000 + ts.nsec / 1000000;
    }
    return sys_poll(ufds, nfds, timeout_ms);
}

/* select(2): bitmap-based readiness over [0,nfds). Up to 256 fds (4 words). */
static int64_t do_select(int nfds, uint64_t ur, uint64_t uw, uint64_t ue, int64_t timeout_ms) {
    if (nfds < 0 || nfds > 256) return -EINVAL_;
    int words = (nfds + 63) / 64;
    uint64_t inr[4] = {0}, inw[4] = {0}, ine[4] = {0};
    if (ur && copy_from_user(inr, cur_vm(), ur, (uint64_t)words * 8) < 0) return -EFAULT_;
    if (uw && copy_from_user(inw, cur_vm(), uw, (uint64_t)words * 8) < 0) return -EFAULT_;
    if (ue && copy_from_user(ine, cur_vm(), ue, (uint64_t)words * 8) < 0) return -EFAULT_;
    uint64_t start = timer_ticks();
    for (;;) {
        uint64_t outr[4] = {0}, outw[4] = {0}, oute[4] = {0};
        int count = 0;
        for (int fd = 0; fd < nfds; fd++) {
            uint64_t bit = 1ULL << (fd & 63); int w = fd >> 6;
            int wr = (inr[w] & bit) != 0, ww = (inw[w] & bit) != 0, we = (ine[w] & bit) != 0;
            if (!(wr || ww || we)) continue;
            int rdy = fd_ready(fd);
            if (rdy & POLLNVAL_) return -EBADF_;
            if (wr && (rdy & (POLLIN_ | POLLHUP_)))   { outr[w] |= bit; count++; }
            if (ww && (rdy & POLLOUT_))               { outw[w] |= bit; count++; }
            if (we && (rdy & POLLERR_))               { oute[w] |= bit; count++; }
        }
        if (count || timeout_ms == 0) {
            if (ur && copy_to_user(cur_vm(), ur, outr, (uint64_t)words * 8) < 0) return -EFAULT_;
            if (uw && copy_to_user(cur_vm(), uw, outw, (uint64_t)words * 8) < 0) return -EFAULT_;
            if (ue && copy_to_user(cur_vm(), ue, oute, (uint64_t)words * 8) < 0) return -EFAULT_;
            return count;
        }
        int wq = poll_wait_quantum(timeout_ms, start);
        if (wq < 0) return wq;                                  /* -EINTR */
        if (wq > 0) {                                           /* timed out: clear sets */
            uint64_t z[4] = {0};
            if (ur && copy_to_user(cur_vm(), ur, z, (uint64_t)words * 8) < 0) return -EFAULT_;
            if (uw && copy_to_user(cur_vm(), uw, z, (uint64_t)words * 8) < 0) return -EFAULT_;
            if (ue && copy_to_user(cur_vm(), ue, z, (uint64_t)words * 8) < 0) return -EFAULT_;
            return 0;
        }
    }
}

static int64_t sys_select(int nfds, uint64_t ur, uint64_t uw, uint64_t ue, uint64_t utv) {
    int64_t timeout_ms = -1;
    if (utv) {
        struct { int64_t sec, usec; } tv;
        if (copy_from_user(&tv, cur_vm(), utv, sizeof tv) < 0) return -EFAULT_;
        timeout_ms = tv.sec * 1000 + tv.usec / 1000;
    }
    return do_select(nfds, ur, uw, ue, timeout_ms);
}

static int64_t sys_pselect6(int nfds, uint64_t ur, uint64_t uw, uint64_t ue, uint64_t uts) {
    int64_t timeout_ms = -1;
    if (uts) {
        struct { int64_t sec, nsec; } ts;
        if (copy_from_user(&ts, cur_vm(), uts, sizeof ts) < 0) return -EFAULT_;
        timeout_ms = ts.sec * 1000 + ts.nsec / 1000000;
    }
    return do_select(nfds, ur, uw, ue, timeout_ms);   /* sigmask ignored */
}

/* ---- eventfd / timerfd ---- */

static int64_t eventfd_read(file_t *f, uint64_t ubuf, uint64_t n) {
    if (n < 8) return -EINVAL_;
    for (;;) {
        spin_lock(&f->off_lock);
        uint64_t c = f->aux1;
        if (c != 0) {
            uint64_t out = (f->dev & EFD_SEMAPHORE) ? 1 : c;
            f->aux1 = (f->dev & EFD_SEMAPHORE) ? c - 1 : 0;
            spin_unlock(&f->off_lock);
            if (copy_to_user(cur_vm(), ubuf, &out, 8) < 0) return -EFAULT_;
            return 8;
        }
        spin_unlock(&f->off_lock);
        if (f->flags & O_NONBLOCK_) return -EAGAIN_;
        if (signals_pending_current()) return -EINTR_;
        ksleep_ms(5);
    }
}

static int64_t eventfd_write(file_t *f, uint64_t ubuf, uint64_t n) {
    if (n < 8) return -EINVAL_;
    uint64_t v;
    if (copy_from_user(&v, cur_vm(), ubuf, 8) < 0) return -EFAULT_;
    if (v == 0xFFFFFFFFFFFFFFFFULL) return -EINVAL_;
    for (;;) {
        spin_lock(&f->off_lock);
        if (f->aux1 <= 0xFFFFFFFFFFFFFFFEULL - v) {            /* fits below the max */
            f->aux1 += v;
            spin_unlock(&f->off_lock);
            return 8;
        }
        spin_unlock(&f->off_lock);
        if (f->flags & O_NONBLOCK_) return -EAGAIN_;
        if (signals_pending_current()) return -EINTR_;
        ksleep_ms(5);
    }
}

/* Expirations elapsed since the last read; advances the schedule. off_lock held. */
static uint64_t timerfd_expirations_locked(file_t *f) {
    if (f->aux1 == 0) return 0;                                /* disarmed */
    uint64_t now = timer_ticks();
    if (now < f->aux1) return 0;
    if (f->aux2 == 0) { f->aux1 = 0; return 1; }               /* one-shot */
    uint64_t exp = 1 + (now - f->aux1) / f->aux2;
    f->aux1 += exp * f->aux2;
    return exp;
}

static int64_t timerfd_read(file_t *f, uint64_t ubuf, uint64_t n) {
    if (n < 8) return -EINVAL_;
    for (;;) {
        spin_lock(&f->off_lock);
        uint64_t exp = timerfd_expirations_locked(f);
        spin_unlock(&f->off_lock);
        if (exp) {
            if (copy_to_user(cur_vm(), ubuf, &exp, 8) < 0) return -EFAULT_;
            return 8;
        }
        if (f->flags & O_NONBLOCK_) return -EAGAIN_;
        if (signals_pending_current()) return -EINTR_;
        ksleep_ms(5);
    }
}

static int64_t sys_eventfd(unsigned initval, int flags) {
    file_t *f = file_alloc();
    if (!f) return -ENFILE_;
    f->kind = FD_EVENT;
    f->aux1 = initval;
    f->dev  = (flags & EFD_SEMAPHORE) ? EFD_SEMAPHORE : 0;
    f->flags = (flags & O_NONBLOCK_) ? O_NONBLOCK_ : 0;
    int fd = fd_install(f);
    if (fd < 0) { file_unref(f); return fd; }
    if (flags & O_CLOEXEC) task_current()->files->cloexec[fd] = 1;
    return fd;
}

static int64_t sys_timerfd_create(int clockid, int flags) {
    (void)clockid;                                  /* REALTIME vs MONOTONIC: ticks either way */
    file_t *f = file_alloc();
    if (!f) return -ENFILE_;
    f->kind = FD_TIMER;
    f->aux1 = 0; f->aux2 = 0;
    f->flags = (flags & O_NONBLOCK_) ? O_NONBLOCK_ : 0;
    int fd = fd_install(f);
    if (fd < 0) { file_unref(f); return fd; }
    if (flags & O_CLOEXEC) task_current()->files->cloexec[fd] = 1;
    return fd;
}

/* timespec -> ticks (round a nonzero interval up to >= 1 tick so it can fire). */
static uint64_t ts_to_ticks(int64_t sec, int64_t nsec) {
    uint32_t hz = timer_hz() ? timer_hz() : 100;
    uint64_t t = (uint64_t)sec * hz + ((uint64_t)nsec * hz) / 1000000000ULL;
    if (t == 0 && (sec || nsec)) t = 1;
    return t;
}
static void ticks_to_ts(uint64_t t, int64_t *sec, int64_t *nsec) {
    uint32_t hz = timer_hz() ? timer_hz() : 100;
    *sec = (int64_t)(t / hz);
    *nsec = (int64_t)((t % hz) * (1000000000ULL / hz));
}

#define TFD_TIMER_ABSTIME 1
static int64_t sys_timerfd_settime(int fd, int flags, uint64_t unew, uint64_t uold) {
    file_t *f = fd_get(fd);
    if (!f || f->kind != FD_TIMER) return -EBADF_;
    struct { int64_t it_sec, it_nsec, val_sec, val_nsec; } nv;
    if (copy_from_user(&nv, cur_vm(), unew, sizeof nv) < 0) return -EFAULT_;

    spin_lock(&f->off_lock);
    if (uold) {                                     /* report the previous setting */
        struct { int64_t it_sec, it_nsec, val_sec, val_nsec; } ov;
        uint64_t now = timer_ticks();
        uint64_t rem = (f->aux1 > now) ? f->aux1 - now : 0;
        ticks_to_ts(rem, &ov.val_sec, &ov.val_nsec);
        ticks_to_ts(f->aux2, &ov.it_sec, &ov.it_nsec);
        spin_unlock(&f->off_lock);
        if (copy_to_user(cur_vm(), uold, &ov, sizeof ov) < 0) return -EFAULT_;
        spin_lock(&f->off_lock);
    }
    if (nv.val_sec == 0 && nv.val_nsec == 0) {
        f->aux1 = 0; f->aux2 = 0;                   /* disarm */
    } else {
        uint64_t delay = ts_to_ticks(nv.val_sec, nv.val_nsec);
        if (flags & TFD_TIMER_ABSTIME) {
            uint64_t abs = ts_to_ticks(nv.val_sec, nv.val_nsec);
            uint64_t now = timer_ticks();
            f->aux1 = abs > now ? abs : now + 1;    /* clamp past deadlines to "soon" */
        } else {
            f->aux1 = timer_ticks() + delay;
        }
        f->aux2 = ts_to_ticks(nv.it_sec, nv.it_nsec);
    }
    spin_unlock(&f->off_lock);
    return 0;
}

static int64_t sys_timerfd_gettime(int fd, uint64_t ucur) {
    file_t *f = fd_get(fd);
    if (!f || f->kind != FD_TIMER) return -EBADF_;
    struct { int64_t it_sec, it_nsec, val_sec, val_nsec; } cv;
    spin_lock(&f->off_lock);
    uint64_t now = timer_ticks();
    uint64_t rem = (f->aux1 > now) ? f->aux1 - now : 0;
    ticks_to_ts(rem, &cv.val_sec, &cv.val_nsec);
    ticks_to_ts(f->aux2, &cv.it_sec, &cv.it_nsec);
    spin_unlock(&f->off_lock);
    if (copy_to_user(cur_vm(), ucur, &cv, sizeof cv) < 0) return -EFAULT_;
    return 0;
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
    case 25:  return (uint64_t)sys_mremap(f->rdi, f->rsi, f->rdx, (int)f->r10,
                                          f->r8);                  /* mremap */
    case 13:  return (uint64_t)sys_rt_sigaction((int)f->rdi, (const void *)f->rsi,
                                                (void *)f->rdx, f->r10);  /* rt_sigaction */
    case 14:  return (uint64_t)sys_rt_sigprocmask((int)f->rdi, (const void *)f->rsi,
                                                  (void *)f->rdx, f->r10); /* rt_sigprocmask */
    case 15:  return signals_sigreturn(f);                     /* rt_sigreturn */
    case 16:  return (uint64_t)sys_ioctl((int)f->rdi, (unsigned)f->rsi, f->rdx);  /* ioctl */
    case 19:  return (uint64_t)sys_readv((int)f->rdi, (const struct iovec *)f->rsi,
                                         (int)f->rdx);         /* readv */
    case 20:  return (uint64_t)sys_writev((int)f->rdi, (const struct iovec *)f->rsi,
                                          (int)f->rdx);        /* writev */
    case 22:  return (uint64_t)sys_pipe((int *)f->rdi);        /* pipe */
    case 28:  return 0;                                        /* madvise (stub) */
    case 32:  return (uint64_t)sys_dup((int)f->rdi);           /* dup */
    case 33:  return (uint64_t)sys_dup2((int)f->rdi, (int)f->rsi);  /* dup2 */
    case 39:  return (uint64_t)task_tgid(task_current());      /* getpid (== tgid) */
    case 102: return 0;                                        /* getuid  (single-user root) */
    case 104: return 0;                                        /* getgid */
    case 107: return 0;                                        /* geteuid */
    case 108: return 0;                                        /* getegid */
    case 105:                                                  /* setuid */
    case 106: return f->rdi == 0 ? 0                           /* setgid: only root (0) is real */
                                 : (uint64_t)(int64_t)-EPERM_;
    case 109: return (uint64_t)task_setpgid((int)f->rdi, (int)f->rsi);  /* setpgid */
    case 121: return (uint64_t)task_getpgid((int)f->rdi);      /* getpgid */
    case 111: return (uint64_t)task_getpgid(0);                /* getpgrp */
    case 112: return (uint64_t)task_setsid();                  /* setsid */
    case 124: return (uint64_t)task_getsid((int)f->rdi);       /* getsid */
    case 56:  return (uint64_t)sys_clone(f);                   /* clone */
    case 202: return (uint64_t)sys_futex(f->rdi, (int)f->rsi, (int)f->rdx);  /* futex */
    case 7:   return (uint64_t)sys_poll(f->rdi, (uint32_t)f->rsi, (int64_t)(int)f->rdx);  /* poll */
    case 271: return (uint64_t)sys_ppoll(f->rdi, (uint32_t)f->rsi, f->rdx);  /* ppoll */
    case 23:  return (uint64_t)sys_select((int)f->rdi, f->rsi, f->rdx, f->r10, f->r8); /* select */
    case 270: return (uint64_t)sys_pselect6((int)f->rdi, f->rsi, f->rdx, f->r10, f->r8); /* pselect6 */
    case 284: return (uint64_t)sys_eventfd((unsigned)f->rdi, 0);          /* eventfd */
    case 290: return (uint64_t)sys_eventfd((unsigned)f->rdi, (int)f->rsi); /* eventfd2 */
    case 283: return (uint64_t)sys_timerfd_create((int)f->rdi, (int)f->rsi);   /* timerfd_create */
    case 286: return (uint64_t)sys_timerfd_settime((int)f->rdi, (int)f->rsi,
                                                   f->rdx, f->r10);            /* timerfd_settime */
    case 287: return (uint64_t)sys_timerfd_gettime((int)f->rdi, f->rsi);       /* timerfd_gettime */
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
    case 73:  return (uint64_t)sys_flock((int)f->rdi, (int)f->rsi);             /* flock */
    case 95:  return (uint64_t)sys_umask((int)f->rdi);                          /* umask */
    case 98:  return (uint64_t)sys_getrusage((int)f->rdi, f->rsi);              /* getrusage */
    case 110: return (uint64_t)sys_getppid();                                   /* getppid */
    case 130: return (uint64_t)sys_rt_sigsuspend((const void *)f->rdi, f->rsi); /* rt_sigsuspend */
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
    case 273: return 0;                                        /* set_robust_list (accept) */
    case 324: return 0;                                        /* membarrier (BSP-pinned: a no-op) */
    case 1000: return (uint64_t)sys_sysconfig((int)f->rdi, f->rsi, f->rdx, f->r10); /* sysconfig (VibeOS) */
    case 90:  return (uint64_t)sys_chmod((const char *)f->rdi, (uint32_t)f->rsi);   /* chmod */
    case 91:  return (uint64_t)sys_fchmod((int)f->rdi, (uint32_t)f->rsi);           /* fchmod */
    case 268: return (uint64_t)sys_chmod((const char *)f->rsi, (uint32_t)f->rdx);  /* fchmodat(dirfd,path,mode,flag) */
    case 88:  return (uint64_t)sys_symlink((const char *)f->rdi, (const char *)f->rsi); /* symlink */
    case 89:  return (uint64_t)sys_readlink((const char *)f->rdi, (char *)f->rsi, f->rdx); /* readlink */
    case 267: return (uint64_t)sys_readlink((const char *)f->rsi, (char *)f->rdx, f->r10); /* readlinkat */
    case 87:  return (uint64_t)sys_unlink((const char *)f->rdi);        /* unlink */
    case 263: return (uint64_t)sys_unlinkat((int)f->rdi, (const char *)f->rsi,
                                            (int)f->rdx);              /* unlinkat */
    case 84:  return (uint64_t)sys_rmdir((const char *)f->rdi);         /* rmdir */
    case 82:  return (uint64_t)sys_rename((const char *)f->rdi,
                                          (const char *)f->rsi);       /* rename */
    case 264: return (uint64_t)sys_rename((const char *)f->rsi,
                                          (const char *)f->r10);       /* renameat(olddir,old,newdir,new) */
    case 316: return f->r8 ? (uint64_t)(int64_t)-EINVAL_                /* renameat2: no flags supported */
                           : (uint64_t)sys_rename((const char *)f->rsi, (const char *)f->r10);
    case 76:  return (uint64_t)sys_truncate((const char *)f->rdi, (int64_t)f->rsi);  /* truncate */
    case 77:  return (uint64_t)sys_ftruncate((int)f->rdi, (int64_t)f->rsi);          /* ftruncate */
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
    case 218:                                                  /* set_tid_address */
        task_current()->clear_child_tid = f->rdi;
        return (uint64_t)task_current()->id;
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
        const char *vals[6] = { "VibeOS",
                                config_get_def("hostname", "vibeos"),   /* /config */
                                "0.9", "VibeOS x86_64 SMP", "x86_64",
                                config_get_def("domainname", "(none)") };
        char *flds[6] = { u.sysname, u.nodename, u.release, u.version, u.machine, u.domainname };
        for (int i = 0; i < 6; i++)
            for (int j = 0; vals[i][j] && j < 64; j++) flds[i][j] = vals[i][j];
        if (copy_to_user(cur_vm(), f->rdi, &u, sizeof u) < 0) return (uint64_t)(int64_t)-EFAULT_;
        return 0;
    }
    case 228: {                                                /* clock_gettime(clockid, ts) */
        uint64_t sec, nsec;
        switch ((int)f->rdi) {
        case 1: case 4: case 6: case 7: {                      /* MONOTONIC[_RAW/_COARSE]/BOOTTIME */
            uint32_t hz = timer_hz() ? timer_hz() : 100;
            uint64_t t = timer_ticks();
            sec = t / hz; nsec = (t % hz) * (1000000000ULL / hz);
            break;
        }
        default:                                               /* REALTIME[_COARSE] + fallback */
            rtc_realtime(&sec, &nsec);
            break;
        }
        struct { int64_t sec, nsec; } ts = { (int64_t)sec, (int64_t)nsec };
        if (copy_to_user(cur_vm(), f->rsi, &ts, sizeof ts) < 0) return (uint64_t)(int64_t)-EFAULT_;
        return 0;
    }
    case 96: {                                                 /* gettimeofday(tv, tz) */
        uint64_t sec, nsec;
        rtc_realtime(&sec, &nsec);
        struct { int64_t sec, usec; } tv = { (int64_t)sec, (int64_t)(nsec / 1000) };
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
        if (task_current()->is_thread) sys_exit_thread();      /* thread: no zombie */
        sys_exit((int)f->rdi);
    case 231: sys_exit((int)f->rdi);                           /* exit_group: tear down */
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
