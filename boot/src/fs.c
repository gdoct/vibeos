#include "fs.h"
#include "util.h"

static EFI_GUID gLoadedImageGuid       = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID gSimpleFileSystemGuid  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gFileInfoGuid          = EFI_FILE_INFO_GUID;

EFI_STATUS fs_read_file(CHAR16 *path, VOID **buffer_out, UINTN *size_out) {
    EFI_STATUS s;

    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    s = gBS->HandleProtocol(gImageHandle, &gLoadedImageGuid, (VOID **)&loaded);
    if (EFI_ERROR(s)) return s;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    s = gBS->HandleProtocol(loaded->DeviceHandle, &gSimpleFileSystemGuid, (VOID **)&fs);
    if (EFI_ERROR(s)) return s;

    EFI_FILE_PROTOCOL *root = 0;
    s = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(s)) return s;

    EFI_FILE_PROTOCOL *file = 0;
    s = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) {
        root->Close(root);
        return s;
    }

    /* Query file size via EFI_FILE_INFO. Buffer must be sized dynamically
       because FileName is variable length, so we ask once with a too-small
       size to learn the right size and then ask again. */
    UINTN info_size = 0;
    s = file->GetInfo(file, &gFileInfoGuid, &info_size, 0);
    if (s != EFI_BUFFER_TOO_SMALL) {
        file->Close(file);
        root->Close(root);
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    }
    VOID *info_buf = 0;
    s = gBS->AllocatePool(EfiLoaderData, info_size, &info_buf);
    if (EFI_ERROR(s)) {
        file->Close(file);
        root->Close(root);
        return s;
    }
    s = file->GetInfo(file, &gFileInfoGuid, &info_size, info_buf);
    if (EFI_ERROR(s)) {
        gBS->FreePool(info_buf);
        file->Close(file);
        root->Close(root);
        return s;
    }
    UINT64 file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
    gBS->FreePool(info_buf);

    VOID *buf = 0;
    s = gBS->AllocatePool(EfiLoaderData, (UINTN)file_size, &buf);
    if (EFI_ERROR(s)) {
        file->Close(file);
        root->Close(root);
        return s;
    }

    UINTN read = (UINTN)file_size;
    s = file->Read(file, &read, buf);
    if (EFI_ERROR(s) || read != file_size) {
        gBS->FreePool(buf);
        file->Close(file);
        root->Close(root);
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    }

    file->Close(file);
    root->Close(root);

    *buffer_out = buf;
    *size_out = (UINTN)file_size;
    return EFI_SUCCESS;
}
