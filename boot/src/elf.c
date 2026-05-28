#include "elf.h"
#include "util.h"
#include "../include/elf.h"

EFI_STATUS elf_load(VOID *image, UINTN image_size,
                    UINT64 *entry_out,
                    UINT64 *phys_lo_out, UINT64 *phys_hi_out) {
    if (image_size < sizeof(Elf64_Ehdr)) return EFI_LOAD_ERROR;

    Elf64_Ehdr *eh = (Elf64_Ehdr *)image;
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        put(u"[elf] bad magic\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA]  != ELFDATA2LSB ||
        eh->e_ident[EI_VERSION] != EV_CURRENT) {
        put(u"[elf] not 64-bit LE current\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_machine != EM_X86_64 || eh->e_type != ET_EXEC) {
        put(u"[elf] not x86_64 ET_EXEC\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0) {
        put(u"[elf] bad program header table\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_phoff + (UINT64)eh->e_phnum * eh->e_phentsize > image_size) {
        put(u"[elf] phdr table out of range\r\n");
        return EFI_LOAD_ERROR;
    }

    Elf64_Phdr *ph = (Elf64_Phdr *)((UINT8 *)image + eh->e_phoff);

    UINT64 lo = (UINT64)-1, hi = 0;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        if (ph[i].p_filesz > ph[i].p_memsz) return EFI_LOAD_ERROR;
        if (ph[i].p_offset + ph[i].p_filesz > image_size) return EFI_LOAD_ERROR;

        UINT64 seg_lo = ph[i].p_paddr & ~(UINT64)(EFI_PAGE_SIZE - 1);
        UINT64 seg_hi = (ph[i].p_paddr + ph[i].p_memsz + EFI_PAGE_SIZE - 1)
                        & ~(UINT64)(EFI_PAGE_SIZE - 1);
        UINTN pages = (UINTN)((seg_hi - seg_lo) / EFI_PAGE_SIZE);

        EFI_PHYSICAL_ADDRESS addr = seg_lo;
        EFI_STATUS s = gBS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(s)) {
            put(u"[elf] AllocatePages failed at ");
            put_hex(seg_lo);
            put(u"\r\n");
            return s;
        }

        UINT8 *dst = (UINT8 *)(UINTN)ph[i].p_paddr;
        bmemcpy(dst, (UINT8 *)image + ph[i].p_offset, (UINTN)ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            bmemset(dst + ph[i].p_filesz, 0,
                    (UINTN)(ph[i].p_memsz - ph[i].p_filesz));
        }

        if (seg_lo < lo) lo = seg_lo;
        if (seg_hi > hi) hi = seg_hi;
    }

    if (lo == (UINT64)-1) {
        put(u"[elf] no PT_LOAD segments\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_entry < lo || eh->e_entry >= hi) {
        put(u"[elf] entry outside loaded range\r\n");
        return EFI_LOAD_ERROR;
    }

    *entry_out = eh->e_entry;
    *phys_lo_out = lo;
    *phys_hi_out = hi;
    return EFI_SUCCESS;
}
