#ifndef MYOS_BOOT_GOP_H
#define MYOS_BOOT_GOP_H

#include "../include/efi.h"
#include "../include/bootinfo.h"

EFI_STATUS gop_init(FramebufferInfo *fb_out);

#endif
