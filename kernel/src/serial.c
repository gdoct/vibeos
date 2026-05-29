#include "kernel.h"
#include "io.h"

/* 8250/16550 UART at COM1 (0x3F8). */
#define COM1 0x3F8

#define UART_DATA   0   /* R/W: data; if DLAB=1, low byte of divisor */
#define UART_IER    1   /* W:  interrupt enable; if DLAB=1, hi byte of divisor */
#define UART_FCR    2   /* W:  FIFO control */
#define UART_LCR    3   /* W:  line control (DLAB lives here) */
#define UART_MCR    4   /* W:  modem control */
#define UART_LSR    5   /* R:  line status */

#define LSR_DR      (1u << 0)   /* data ready (a byte waiting in RBR/FIFO) */
#define LSR_THRE    (1u << 5)

static int serial_ready = 0;

void serial_init(void) {
    outb(COM1 + UART_IER, 0x00);   /* mask all interrupts */
    outb(COM1 + UART_LCR, 0x80);   /* enable DLAB */
    outb(COM1 + UART_DATA, 0x01);  /* divisor lo = 1 → 115200 baud */
    outb(COM1 + UART_IER,  0x00);  /* divisor hi = 0 */
    outb(COM1 + UART_LCR, 0x03);   /* 8N1, DLAB off */
    outb(COM1 + UART_FCR, 0xC7);   /* enable FIFO, clear, 14-byte threshold */
    outb(COM1 + UART_MCR, 0x0B);   /* DTR + RTS + OUT2 */
    serial_ready = 1;
}

void serial_putc(char c) {
    if (!serial_ready) return;
    while ((inb(COM1 + UART_LSR) & LSR_THRE) == 0) { }
    outb(COM1 + UART_DATA, (uint8_t)c);
}

void serial_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') serial_putc('\r');
        serial_putc(s[i]);
    }
}

/* Receive side. We don't use the UART RX interrupt; the TTY polls these from
   the timer tick (see tty_poll), which is plenty for interactive typing and
   avoids routing an ISA IRQ through the I/O APIC. */
int serial_rx_ready(void) {
    return serial_ready && (inb(COM1 + UART_LSR) & LSR_DR);
}

char serial_getc(void) {
    return (char)inb(COM1 + UART_DATA);
}
