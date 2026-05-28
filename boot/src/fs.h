#ifndef MYOS_BOOT_FS_H
#define MYOS_BOOT_FS_H

#include "../include/efi.h"

EFI_STATUS fs_read_file(CHAR16 *path, VOID **buffer, UINTN *size);

#endif
