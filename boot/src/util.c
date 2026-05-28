#include "util.h"

EFI_SYSTEM_TABLE *gST = 0;
EFI_BOOT_SERVICES *gBS = 0;
EFI_HANDLE gImageHandle = 0;

void put(const CHAR16 *s) {
    if (gST && gST->ConOut) {
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s);
    }
}

void put_hex(UINT64 v) {
    CHAR16 buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned nyb = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = nyb < 10 ? (CHAR16)('0' + nyb) : (CHAR16)('a' + nyb - 10);
    }
    buf[18] = 0;
    put(buf);
}

void put_dec(UINT64 v) {
    CHAR16 buf[21];
    int i = 20;
    buf[i--] = 0;
    if (v == 0) {
        buf[i--] = '0';
    } else {
        while (v && i >= 0) {
            buf[i--] = (CHAR16)('0' + (v % 10));
            v /= 10;
        }
    }
    put(&buf[i + 1]);
}

void panic(const CHAR16 *msg, EFI_STATUS status) {
    put(u"\r\n[boot] PANIC: ");
    put(msg);
    put(u" status=");
    put_hex(status);
    put(u"\r\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void *bmemset(void *dst, int c, UINTN n) {
    UINT8 *d = dst;
    for (UINTN i = 0; i < n; i++) d[i] = (UINT8)c;
    return dst;
}

void *bmemcpy(void *dst, const void *src, UINTN n) {
    UINT8 *d = dst;
    const UINT8 *s = src;
    for (UINTN i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

int bmemcmp(const void *a, const void *b, UINTN n) {
    const UINT8 *x = a, *y = b;
    for (UINTN i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}
