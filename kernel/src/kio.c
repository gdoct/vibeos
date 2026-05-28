#include "kernel.h"

void kputc(char c) {
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}

void kputs(const char *s) {
    while (*s) kputc(*s++);
}

static void put_uint(uint64_t v, unsigned base, int width, char pad, int upper) {
    char buf[32];
    int  i = 0;
    if (v == 0) {
        buf[i++] = '0';
    } else {
        const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        while (v) {
            buf[i++] = digits[v % base];
            v /= base;
        }
    }
    while (i < width) buf[i++] = pad;
    while (i--) kputc(buf[i]);
}

void kvprintf(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputc(*fmt); continue; }
        fmt++;

        char pad = ' ';
        int  width = 0;
        int  left  = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt - '0'); fmt++; }

        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; }
        if (*fmt == 'z') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)kstrlen(s);
            int pad_n = width > len ? width - len : 0;
            if (!left) while (pad_n--) kputc(' ');
            kputs(s);
            if (left)  while (pad_n-- > 0) kputc(' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            if (v < 0) { kputc('-'); v = -v; }
            put_uint((uint64_t)v, 10, width, pad, 0);
            break;
        }
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            put_uint(v, 10, width, pad, 0);
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            put_uint(v, 16, width, pad, 0);
            break;
        }
        case 'X': {
            uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            put_uint(v, 16, width, pad, 1);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            kputs("0x");
            put_uint(v, 16, 16, '0', 0);
            break;
        }
        case '%': kputc('%'); break;
        default:  kputc('%'); kputc(*fmt); break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
