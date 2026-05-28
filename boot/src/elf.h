#ifndef MYOS_BOOT_ELFLOADER_H
#define MYOS_BOOT_ELFLOADER_H

#include "../include/efi.h"

EFI_STATUS elf_load(VOID *image, UINTN image_size,
                    UINT64 *entry_out,
                    UINT64 *phys_lo_out, UINT64 *phys_hi_out);

#endif
