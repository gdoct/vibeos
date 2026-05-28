#include "kernel.h"
#include "io.h"

/*
 * 8259A Programmable Interrupt Controller (PIC).
 *
 * Two cascaded PICs: master at 0x20/0x21, slave at 0xA0/0xA1. The slave's
 * interrupt line feeds the master's IRQ2. After UEFI, the BIOS-default
 * mapping has the master at vectors 8..15, which overlaps CPU exceptions —
 * so we remap to 0x20..0x2F before unmasking anything.
 */

#define PIC1_CMD     0x20
#define PIC1_DATA    0x21
#define PIC2_CMD     0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

#define PIC_EOI      0x20

extern "C" {

void pic_remap(uint8_t off1, uint8_t off2) {
    /* Start init sequence (cascaded, expect ICW4). */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offsets. */
    outb(PIC1_DATA, off1); io_wait();
    outb(PIC2_DATA, off2); io_wait();

    /* ICW3: tell master that slave is at IRQ2 (0x04 = bit 2 set);
       tell slave its cascade identity (= 2). */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* OCW1: mask everything. Drivers unmask the lines they care about. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq       : (irq - 8);
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq       : (irq - 8);
    outb(port, (uint8_t)(inb(port) & ~(1u << bit)));
    /* A slave line (8..15) only reaches the CPU if the cascade (master
       IRQ2) is also open. Unmask it whenever we enable any slave line. */
    if (irq >= 8)
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
}

}  /* extern "C" */
