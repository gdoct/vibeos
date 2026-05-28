#include "gop.h"
#include "util.h"

static EFI_GUID gGopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

EFI_STATUS gop_init(FramebufferInfo *fb_out) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS s = gBS->LocateProtocol(&gGopGuid, 0, (VOID **)&gop);
    if (EFI_ERROR(s)) return s;

    /*
     * Pick the highest-resolution mode whose pixel format is a flat
     * 32-bit BGR or RGB layout. We skip PixelBitMask / PixelBltOnly
     * modes — they require translation work the kernel doesn't need
     * during early boot.
     */
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_pixels = gop->Mode->Info->HorizontalResolution
                       * gop->Mode->Info->VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT best_fmt = gop->Mode->Info->PixelFormat;
    int have_linear = (best_fmt == PixelBlueGreenRedReserved8BitPerColor)
                   || (best_fmt == PixelRedGreenBlueReserved8BitPerColor);
    if (!have_linear) best_pixels = 0;

    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN info_size = 0;
        if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) continue;
        int linear = (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
                  || (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor);
        if (!linear) continue;
        UINT32 pixels = info->HorizontalResolution * info->VerticalResolution;
        if (pixels > best_pixels) {
            best_pixels = pixels;
            best_mode = i;
            best_fmt = info->PixelFormat;
        }
    }

    if (best_mode != gop->Mode->Mode) {
        s = gop->SetMode(gop, best_mode);
        if (EFI_ERROR(s)) return s;
    }

    fb_out->base   = gop->Mode->FrameBufferBase;
    fb_out->size   = gop->Mode->FrameBufferSize;
    fb_out->width  = gop->Mode->Info->HorizontalResolution;
    fb_out->height = gop->Mode->Info->VerticalResolution;
    fb_out->pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
    fb_out->format = (gop->Mode->Info->PixelFormat
                      == PixelBlueGreenRedReserved8BitPerColor)
                     ? FB_FORMAT_BGR8 : FB_FORMAT_RGB8;
    return EFI_SUCCESS;
}
