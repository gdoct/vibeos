#ifndef VIBEOS_KERNEL_H
#define VIBEOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void *kmemset(void *dst, int c, size_t n);
void *kmemcpy(void *dst, const void *src, size_t n);
int   kmemcmp(const void *a, const void *b, size_t n);
size_t kstrlen(const char *s);

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
int  serial_rx_ready(void);   /* 1 if a received byte is waiting */
char serial_getc(void);       /* read one received byte (call when rx_ready) */

void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);

__attribute__((noreturn))
void panic(const char *fmt, ...);

#define KASSERT(cond) do { \
    if (!(cond)) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
} while (0)

__attribute__((noreturn))
static inline void halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
    __builtin_unreachable();
}

#ifdef __cplusplus
}
#endif

#endif
