#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "fs.h"
#include "task.h"
#include "usermode.h"
#include "elf.h"
#include "random.h"

/*
 * Userspace ELF loader (ROADMAP §3 Phase 2, widened for §4 Linux ABI).
 *
 * Loads a static ET_EXEC into the low half of the *currently active* address
 * space and builds the System V x86_64 initial stack a real libc expects:
 *
 *     rsp -> argc
 *            argv[0..argc-1], NULL
 *            envp[0..],       NULL
 *            auxv pairs ..., { AT_NULL, 0 }
 *            (argv/env strings, 16 AT_RANDOM bytes)
 *
 * §4 changes vs. the milestone-A loader:
 *   - segments are streamed straight off the fd into their user VAs, so the
 *     whole file no longer has to fit in one kmalloc buffer (a static `ld` is
 *     several MB) — there is no image-size cap anymore;
 *   - a real auxv (AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/RANDOM/...), which musl
 *     needs to find its TLS phdr and seed the stack canary;
 *   - argv/envp are passed through from execve instead of fabricated;
 *   - the user stack is 256 KiB (a libc start path touches far more than the
 *     old 32 KiB), and the per-process anonymous-mmap arena is armed.
 *
 * The loader writes through the user VAs directly (they are PTE_U|PTE_W and
 * the address space is the active CR3); copy_*_user validation is still TODO.
 */

#define USER_STACK_TOP    0x40000000ULL          /* 1 GiB */
#define USER_STACK_PAGES  64                      /* 256 KiB */
#define USER_HEAP_MAX     (16ULL * 1024 * 1024)   /* brk window */
#define USER_MMAP_BASE    0x200000000ULL          /* 8 GiB: anonymous mmap arena */

#define MAX_ARGS          64                      /* argv/envp cap per vector */

/* auxv types we populate (subset of <elf.h> AT_*). */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_RANDOM  25

/* Map [va, va+len) as user pages in `vm`, allocating any not already present
   (adjacent segments may share a page). */
static void map_user(vmspace_t *vm, uint64_t va, uint64_t len) {
    uint64_t a = PAGE_ALIGN_DOWN(va);
    uint64_t e = PAGE_ALIGN_UP(va + len);
    for (; a < e; a += PAGE_SIZE) {
        uint64_t dummy;
        if (vmspace_query(vm, a, &dummy)) continue;   /* already mapped */
        uint64_t pa = pmm_alloc_page();
        if (!pa) panic("user_load: out of memory mapping %lx", (unsigned long)a);
        vmspace_map(vm, a, pa, 1, PTE_P | PTE_W | PTE_U);
    }
}

/* Read exactly `n` bytes at file offset `off` into `dst` (which may be a user
   VA in the active address space). Returns 0 on success, <0 on short/failed
   read. */
static int read_exact(int fd, uint64_t off, void *dst, uint64_t n) {
    if (fs_seek(fd, off) < 0) return -1;
    uint8_t *p = (uint8_t *)dst;
    while (n) {
        uint32_t chunk = (n > 0x40000000u) ? 0x40000000u : (uint32_t)n;
        int r = fs_read(fd, p, chunk);
        if (r < 0) return r;
        if (r == 0) return -1;           /* premature EOF */
        p += r; n -= (uint64_t)r;
    }
    return 0;
}

/* Build the System V initial stack in the active address space. argv/envp are
   kernel-side NULL-terminated arrays (reachable here regardless of CR3). */
static void build_initial_stack(const Elf64_Ehdr *eh, const Elf64_Phdr *ph,
                                char *const argv[], char *const envp[],
                                uint64_t *rsp_out) {
    int argc = 0; while (argv && argc < MAX_ARGS && argv[argc]) argc++;
    int envc = 0; while (envp && envc < MAX_ARGS && envp[envc]) envc++;

    uint64_t argp[MAX_ARGS], envpp[MAX_ARGS];
    uint64_t sp = USER_STACK_TOP;

    for (int i = 0; i < argc; i++) {
        uint64_t len = (uint64_t)kstrlen(argv[i]) + 1;
        sp -= len; kmemcpy((void *)(uintptr_t)sp, argv[i], len); argp[i] = sp;
    }
    for (int i = 0; i < envc; i++) {
        uint64_t len = (uint64_t)kstrlen(envp[i]) + 1;
        sp -= len; kmemcpy((void *)(uintptr_t)sp, envp[i], len); envpp[i] = sp;
    }

    /* 16 bytes of entropy for AT_RANDOM (musl's stack canary). */
    sp -= 16;
    uint64_t rnd_va = sp;
    krandom_bytes((void *)(uintptr_t)sp, 16);

    sp &= ~0xFULL;   /* end of the string/aux-data area */

    /* Locate the program headers in memory for AT_PHDR. */
    uint64_t phdr_va = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD &&
            eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff <  ph[i].p_offset + ph[i].p_filesz) {
            phdr_va = ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
            break;
        }
    }

    struct { uint64_t type, val; } aux[] = {
        { AT_PHDR,   phdr_va },
        { AT_PHENT,  sizeof(Elf64_Phdr) },
        { AT_PHNUM,  eh->e_phnum },
        { AT_PAGESZ, PAGE_SIZE },
        { AT_ENTRY,  eh->e_entry },
        { AT_RANDOM, rnd_va },
        { AT_UID,    0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 },
        { AT_SECURE, 0 }, { AT_CLKTCK, 100 },
        { AT_NULL,   0 },
    };
    int naux = (int)(sizeof(aux) / sizeof(aux[0]));

    /* words: argc + (argc+1) argv + (envc+1) envp + 2*naux auxv. */
    int nwords = 1 + (argc + 1) + (envc + 1) + 2 * naux;
    uint64_t vec = (sp - (uint64_t)nwords * 8) & ~0xFULL;   /* 16-align argc */
    uint64_t *w = (uint64_t *)(uintptr_t)vec;
    int k = 0;
    w[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) w[k++] = argp[i];
    w[k++] = 0;
    for (int i = 0; i < envc; i++) w[k++] = envpp[i];
    w[k++] = 0;
    for (int i = 0; i < naux; i++) { w[k++] = aux[i].type; w[k++] = aux[i].val; }

    *rsp_out = vec;
}

int user_load_path(vmspace_t *vm, const char *path,
                   char *const argv[], char *const envp[],
                   uint64_t *entry_out, uint64_t *rsp_out) {
    int fd = fs_open(path, 0);
    if (fd < 0) return fd;                       /* FS errno (e.g. -ENOENT) */

    Elf64_Ehdr eh;
    if (read_exact(fd, 0, &eh, sizeof eh) < 0) { fs_close(fd); return -1; }
    if (eh.e_ident[0] != ELFMAG0 || eh.e_ident[1] != ELFMAG1 ||
        eh.e_ident[2] != ELFMAG2 || eh.e_ident[3] != ELFMAG3) { fs_close(fd); return -2; }
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA]  != ELFDATA2LSB) { fs_close(fd); return -3; }
    if (eh.e_type != ET_EXEC || eh.e_machine != EM_X86_64) { fs_close(fd); return -4; }
    if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0) { fs_close(fd); return -5; }

    uint64_t phsz = (uint64_t)eh.e_phnum * eh.e_phentsize;
    Elf64_Phdr *ph = (Elf64_Phdr *)kmalloc(phsz);
    if (!ph) { fs_close(fd); return -12; }
    if (read_exact(fd, eh.e_phoff, ph, phsz) < 0) { kfree(ph); fs_close(fd); return -1; }

    uint64_t brk_end = 0;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_filesz > ph[i].p_memsz) { kfree(ph); fs_close(fd); return -6; }
        if (ph[i].p_vaddr >= PHYS_OFFSET)    { kfree(ph); fs_close(fd); return -8; }

        map_user(vm, ph[i].p_vaddr, ph[i].p_memsz);
        /* Pages come zeroed from the PMM, so the .bss tail is already clear;
           stream just the file-backed bytes straight into the user VA. */
        if (ph[i].p_filesz &&
            read_exact(fd, ph[i].p_offset,
                       (void *)(uintptr_t)ph[i].p_vaddr, ph[i].p_filesz) < 0) {
            kfree(ph); fs_close(fd); return -7;
        }
        uint64_t end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end > brk_end) brk_end = end;
    }
    fs_close(fd);

    /* Stack. */
    uint64_t stk_lo = USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    map_user(vm, stk_lo, USER_STACK_TOP - stk_lo);
    build_initial_stack(&eh, ph, argv, envp, rsp_out);
    kfree(ph);

    /* Heap (brk) starts after the highest segment; arm the mmap arena. */
    uint64_t brk_start = PAGE_ALIGN_UP(brk_end);
    user_heap_init(brk_start, brk_start + USER_HEAP_MAX);
    task_current()->mmap_next = USER_MMAP_BASE;

    *entry_out = eh.e_entry;
    return 0;
}
