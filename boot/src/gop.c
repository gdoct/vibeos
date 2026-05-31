#include "gop.h"
#include "util.h"

static EFI_GUID gGopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

EFI_STATUS gop_init(FramebufferInfo *fb_out) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS s = gBS->LocateProtocol(&gGopGuid, 0, (VOID **)&gop);
    if (EFI_ERROR(s)) return s;

    /*
     * Prefer our target resolution (PREF_W x PREF_H) when the firmware offers
     * it; otherwise fall back to the highest-resolution mode. Only flat 32-bit
     * BGR/RGB modes are considered — PixelBitMask / PixelBltOnly need translation
     * the kernel doesn't do during early boot.
     */
    const UINT32 PREF_W = 1280, PREF_H = 960;
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_pixels = gop->Mode->Info->HorizontalResolution
                       * gop->Mode->Info->VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT best_fmt = gop->Mode->Info->PixelFormat;
    int have_linear = (best_fmt == PixelBlueGreenRedReserved8BitPerColor)
                   || (best_fmt == PixelRedGreenBlueReserved8BitPerColor);
    if (!have_linear) best_pixels = 0;
    UINT32 pref_mode = 0; int pref_found = 0;

    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN info_size = 0;
        if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) continue;
        int linear = (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
                  || (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor);
        if (!linear) continue;
        if (info->HorizontalResolution == PREF_W && info->VerticalResolution == PREF_H) {
            pref_mode = i; pref_found = 1;        /* exact match — use it */
        }
        UINT32 pixels = info->HorizontalResolution * info->VerticalResolution;
        if (pixels > best_pixels) {
            best_pixels = pixels;
            best_mode = i;
            best_fmt = info->PixelFormat;
        }
    }

    if (pref_found) best_mode = pref_mode;

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
