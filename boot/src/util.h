#ifndef VIBEOS_BOOT_UTIL_H
#define VIBEOS_BOOT_UTIL_H

#include "../include/efi.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_HANDLE gImageHandle;

void put(const CHAR16 *s);
void put_hex(UINT64 v);
void put_dec(UINT64 v);
void panic(const CHAR16 *msg, EFI_STATUS status);

void *bmemset(void *dst, int c, UINTN n);
void *bmemcpy(void *dst, const void *src, UINTN n);
int   bmemcmp(const void *a, const void *b, UINTN n);

#endif
