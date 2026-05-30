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
 * Userspace ELF loader (ROADMAP §3 Phase 2; §4 dynamic linking).
 *
 * Loads a static ET_EXEC, a position-independent ET_DYN (PIE), or a dynamically
 * linked program into the *currently active* address space and builds the
 * System V x86_64 initial stack a real libc expects:
 *
 *     rsp -> argc, argv[], NULL, envp[], NULL, auxv..., {AT_NULL,0}
 *
 * Dynamic linking (§4): if the program carries a PT_INTERP, the named dynamic
 * linker (ld-musl, itself an ET_DYN) is also mapped, at its own base, and we
 * enter *its* entry point. The auxv then describes the main program for the
 * linker: AT_PHDR/PHENT/PHNUM + AT_ENTRY point at the program, AT_BASE at the
 * interpreter. ld-musl self-relocates, loads any DT_NEEDED libraries via
 * file-backed mmap/mprotect, and jumps to the program. ET_DYN images are placed
 * at a fixed base (no per-exec ASLR yet).
 */

#define USER_STACK_TOP    0x40000000ULL          /* 1 GiB */
#define USER_STACK_PAGES  64                      /* 256 KiB */
#define USER_HEAP_MAX     (16ULL * 1024 * 1024)   /* brk window */
#define USER_MMAP_BASE    0x200000000ULL          /* 8 GiB: anonymous/file mmap arena */
#define EXE_DYN_BASE      0x4000000000ULL         /* 256 GiB: PIE main image base */
#define INTERP_BASE       0x4100000000ULL         /* 260 GiB: dynamic linker base */

#define MAX_ARGS          64                      /* argv/envp cap per vector */

/* auxv types we populate (subset of <elf.h> AT_*). */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_EXECFN  31

/* What loading one ELF image produced. */
typedef struct {
    uint64_t base;       /* load bias (0 for ET_EXEC) */
    uint64_t entry;      /* base + e_entry */
    uint64_t phdr_va;    /* program headers in memory (for AT_PHDR) */
    uint16_t phnum, phent;
    uint64_t max_va;     /* highest mapped vaddr (for brk placement) */
    int      has_interp;
    char     interp[160];
} loaded_t;

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

/* Read exactly `n` bytes at file offset `off` into `dst` (may be a user VA in
   the active address space). Returns 0 on success, <0 on short/failed read. */
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

/* Load one ELF (ET_EXEC or ET_DYN) at `want_base` (used only for ET_DYN) into
   `vm`, filling `out`. Returns 0 on success, negative on error. */
static int load_image(vmspace_t *vm, const char *path, uint64_t want_base, loaded_t *out) {
    int fd = fs_open(path, 0);
    if (fd < 0) return fd;

    Elf64_Ehdr eh;
    if (read_exact(fd, 0, &eh, sizeof eh) < 0) { fs_close(fd); return -1; }
    if (eh.e_ident[0] != ELFMAG0 || eh.e_ident[1] != ELFMAG1 ||
        eh.e_ident[2] != ELFMAG2 || eh.e_ident[3] != ELFMAG3) { fs_close(fd); return -2; }
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA]  != ELFDATA2LSB) { fs_close(fd); return -3; }
    if ((eh.e_type != ET_EXEC && eh.e_type != ET_DYN) ||
        eh.e_machine != EM_X86_64) { fs_close(fd); return -4; }
    if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0) { fs_close(fd); return -5; }

    uint64_t base = (eh.e_type == ET_DYN) ? want_base : 0;   /* ET_EXEC is fixed */

    uint64_t phsz = (uint64_t)eh.e_phnum * eh.e_phentsize;
    Elf64_Phdr *ph = (Elf64_Phdr *)kmalloc(phsz);
    if (!ph) { fs_close(fd); return -12; }
    if (read_exact(fd, eh.e_phoff, ph, phsz) < 0) { kfree(ph); fs_close(fd); return -1; }

    kmemset(out, 0, sizeof *out);
    out->base = base;
    out->entry = base + eh.e_entry;
    out->phnum = eh.e_phnum;
    out->phent = eh.e_phentsize;

    uint64_t max_va = 0;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            uint64_t n = ph[i].p_filesz;
            if (n == 0 || n > sizeof out->interp) { kfree(ph); fs_close(fd); return -9; }
            if (read_exact(fd, ph[i].p_offset, out->interp, n) < 0) {
                kfree(ph); fs_close(fd); return -1;
            }
            out->interp[n - 1 < sizeof out->interp - 1 ? n - 1 : sizeof out->interp - 1] = '\0';
            out->has_interp = 1;
            continue;
        }
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_filesz > ph[i].p_memsz) { kfree(ph); fs_close(fd); return -6; }
        uint64_t va = base + ph[i].p_vaddr;
        if (va >= PHYS_OFFSET || va + ph[i].p_memsz >= PHYS_OFFSET) {
            kfree(ph); fs_close(fd); return -8;       /* would escape the user half */
        }
        map_user(vm, va, ph[i].p_memsz);
        /* Pages are zeroed by the PMM, so the .bss tail is already clear; stream
           the file-backed bytes straight into the user VA. */
        if (ph[i].p_filesz &&
            read_exact(fd, ph[i].p_offset, (void *)(uintptr_t)va, ph[i].p_filesz) < 0) {
            kfree(ph); fs_close(fd); return -7;
        }
        if (va + ph[i].p_memsz > max_va) max_va = va + ph[i].p_memsz;

        /* Program headers live inside whichever PT_LOAD spans e_phoff (the first
           one, at file offset 0). Record their runtime address for AT_PHDR. */
        if (eh.e_phoff >= ph[i].p_offset &&
            eh.e_phoff < ph[i].p_offset + ph[i].p_filesz)
            out->phdr_va = va + (eh.e_phoff - ph[i].p_offset);
    }
    out->max_va = max_va;
    kfree(ph);
    fs_close(fd);
    return 0;
}

/* Build the System V initial stack in the active address space. argv/envp are
   kernel-side NULL-terminated arrays (reachable here regardless of CR3). */
static void build_initial_stack(const loaded_t *exe, uint64_t interp_base,
                                const char *execfn,
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
    uint64_t execfn_len = (uint64_t)kstrlen(execfn) + 1;
    sp -= execfn_len; kmemcpy((void *)(uintptr_t)sp, execfn, execfn_len);
    uint64_t execfn_va = sp;

    /* 16 bytes of entropy for AT_RANDOM (musl's stack canary). */
    sp -= 16;
    uint64_t rnd_va = sp;
    krandom_bytes((void *)(uintptr_t)sp, 16);

    sp &= ~0xFULL;   /* end of the string/aux-data area */

    struct { uint64_t type, val; } aux[] = {
        { AT_PHDR,   exe->phdr_va },
        { AT_PHENT,  exe->phent },
        { AT_PHNUM,  exe->phnum },
        { AT_PAGESZ, PAGE_SIZE },
        { AT_BASE,   interp_base },        /* 0 for a static image */
        { AT_ENTRY,  exe->entry },
        { AT_RANDOM, rnd_va },
        { AT_EXECFN, execfn_va },
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
    loaded_t exe;
    int r = load_image(vm, path, EXE_DYN_BASE, &exe);
    if (r < 0) return r;

    uint64_t real_entry = exe.entry;
    uint64_t interp_base = 0;
    uint64_t brk_base = exe.max_va;

    if (exe.has_interp) {
        loaded_t interp;
        r = load_image(vm, exe.interp, INTERP_BASE, &interp);
        if (r < 0) return r;                 /* missing/!bad dynamic linker */
        if (interp.has_interp) return -9;    /* an interpreter must be static */
        real_entry  = interp.entry;          /* enter the dynamic linker first */
        interp_base = interp.base;
        if (interp.max_va > brk_base) brk_base = interp.max_va;
    }

    /* Stack, with a deliberately-unmapped guard page below it (ROADMAP §1.1). */
    uint64_t stk_lo = USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    map_user(vm, stk_lo, USER_STACK_TOP - stk_lo);
    build_initial_stack(&exe, interp_base, path, argv, envp, rsp_out);

    /* Heap (brk) starts past the highest mapped image byte; arm the mmap arena
       the dynamic linker (and malloc) draw from. */
    uint64_t brk_start = PAGE_ALIGN_UP(brk_base);
    user_heap_init(brk_start, brk_start + USER_HEAP_MAX);
    task_current()->mmap_next = USER_MMAP_BASE;

    *entry_out = real_entry;
    return 0;
}
