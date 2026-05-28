#include "kernel.h"

__attribute__((noreturn))
void panic(const char *fmt, ...) {
    kputs("\n!! KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputc('\n');
    halt_forever();
}
