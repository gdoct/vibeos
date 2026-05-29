#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "usermode.h"
#include "elf.h"

/*
 * Userspace ELF loader (ROADMAP §3, Phase 2).
 *
 * Loads a static ET_EXEC into the low half of the current address space and
 * builds the System V x86_64 initial stack the program expects at entry:
 *
 *     rsp -> argc
 *            argv[0..argc-1], NULL
 *            envp[0..], NULL
 *            auxv pairs ..., AT_NULL
 *            (strings)
 *
 * Milestone A maps into the shared (kernel) page tables — there is one
 * process — so the kernel can populate segments and the stack by writing
 * straight through the user VAs (they are PTE_U|PTE_W). Per-process address
 * spaces and copy_*_user come in Milestone B / §4. auxv is just AT_NULL for
 * now; §4 adds AT_PHDR/AT_ENTRY/AT_PAGESZ/AT_RANDOM for a real libc.
 */

#define USER_STACK_TOP    0x40000000ULL          /* 1 GiB */
#define USER_STACK_PAGES  8                       /* 32 KiB */
#define USER_HEAP_MAX     (16ULL * 1024 * 1024)   /* brk window */

/* Map [va, va+len) as user pages in `vm`, allocating any pages not already
   present (segments may share a page). */
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

int user_load(vmspace_t *vm, const void *image, uint64_t size,
              const char *argv0, uint64_t *entry_out, uint64_t *rsp_out) {
    if (size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) return -2;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA]  != ELFDATA2LSB) return -3;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) return -4;
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0) return -5;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) return -5;

    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)image + eh->e_phoff);
    uint64_t brk_end = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_filesz > ph[i].p_memsz) return -6;
        if (ph[i].p_offset + ph[i].p_filesz > size) return -7;
        if (ph[i].p_vaddr >= PHYS_OFFSET) return -8;   /* must be low/user half */

        map_user(vm, ph[i].p_vaddr, ph[i].p_memsz);
        /* Pages come zeroed from the PMM, so the .bss tail (memsz > filesz)
           is already clear; just copy the file-backed bytes. */
        kmemcpy((void *)(uintptr_t)ph[i].p_vaddr,
                (const uint8_t *)image + ph[i].p_offset, ph[i].p_filesz);

        uint64_t end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end > brk_end) brk_end = end;
    }

    /* Stack. */
    uint64_t stk_lo = USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    map_user(vm, stk_lo, USER_STACK_TOP - stk_lo);

    /* argv0 string at the very top (16-aligned slot), then the aligned
       argc/argv/envp/auxv vector. Six 8-byte slots keep rsp 16-byte aligned. */
    uint64_t al = (uint64_t)kstrlen(argv0) + 1;
    uint64_t sp = USER_STACK_TOP - ((al + 15) & ~15ULL);
    kmemcpy((void *)(uintptr_t)sp, argv0, al);
    uint64_t arg0 = sp;

    sp -= 6 * 8;
    sp &= ~0xFULL;
    uint64_t *u = (uint64_t *)(uintptr_t)sp;
    u[0] = 1;        /* argc            */
    u[1] = arg0;     /* argv[0]         */
    u[2] = 0;        /* argv NULL       */
    u[3] = 0;        /* envp NULL       */
    u[4] = 0;        /* auxv AT_NULL .. */
    u[5] = 0;        /*    .. value     */

    /* Heap starts after the highest segment. */
    uint64_t brk_start = PAGE_ALIGN_UP(brk_end);
    user_heap_init(brk_start, brk_start + USER_HEAP_MAX);

    *entry_out = eh->e_entry;
    *rsp_out   = sp;
    return 0;
}
