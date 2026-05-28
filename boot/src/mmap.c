#include "mmap.h"
#include "util.h"

EFI_STATUS mmap_exit_boot_services(MemoryMap *mmap_out) {
    EFI_STATUS s;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_MEMORY_DESCRIPTOR *map = 0;

    /* First call sizes the buffer. */
    s = gBS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    if (s != EFI_BUFFER_TOO_SMALL) {
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    }

    /* AllocatePool itself may grow the map, so over-allocate. */
    map_size += desc_size * 8;
    s = gBS->AllocatePool(EfiLoaderData, map_size, (VOID **)&map);
    if (EFI_ERROR(s)) return s;

    /* Try ExitBootServices; on EFI_INVALID_PARAMETER the spec says
       refresh the memory map and retry — but only once. */
    for (int attempt = 0; attempt < 2; attempt++) {
        UINTN cur = map_size;
        s = gBS->GetMemoryMap(&cur, map, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(s)) {
            gBS->FreePool(map);
            return s;
        }
        s = gBS->ExitBootServices(gImageHandle, map_key);
        if (!EFI_ERROR(s)) {
            mmap_out->buffer = (UINT64)(UINTN)map;
            mmap_out->size = cur;
            mmap_out->desc_size = desc_size;
            mmap_out->desc_version = desc_ver;
            mmap_out->pad = 0;
            return EFI_SUCCESS;
        }
        if (s != EFI_INVALID_PARAMETER) break;
    }
    gBS->FreePool(map);
    return s;
}
