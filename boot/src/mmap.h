#ifndef VIBEOS_BOOT_MMAP_H
#define VIBEOS_BOOT_MMAP_H

#include "../include/efi.h"
#include "../include/bootinfo.h"

/*
 * Acquires the firmware memory map, calls ExitBootServices (retrying
 * on EFI_INVALID_PARAMETER as the spec requires), and fills mmap_out.
 * After this returns successfully, boot services are gone and only
 * VibeOS code and runtime services remain.
 */
EFI_STATUS mmap_exit_boot_services(MemoryMap *mmap_out);

#endif
