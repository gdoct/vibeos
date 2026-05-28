#include "kernel.h"
#include "idt.h"

typedef struct {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;          /* bits 0-2: IST index; rest reserved (0) */
    uint8_t  type_attr;    /* P|DPL|0|Type */
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t g_idt[256];
static idt_ptr_t   g_idtr;

extern "C" uint64_t isr_table[32];
extern "C" uint64_t irq_table[16];

static void idt_set_gate(int vec, uint64_t handler) {
    g_idt[vec].offset_lo  = handler & 0xFFFF;
    g_idt[vec].selector   = KERNEL_CS;
    g_idt[vec].ist        = 0;
    g_idt[vec].type_attr  = 0x8E;   /* P=1, DPL=0, Type=0xE interrupt gate */
    g_idt[vec].offset_mid = (handler >> 16) & 0xFFFF;
    g_idt[vec].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    g_idt[vec].reserved   = 0;
}

extern "C" void idt_set_vector(int vec, void *handler) {
    idt_set_gate(vec, (uint64_t)(uintptr_t)handler);
}

void idt_init(void) {
    kmemset(g_idt, 0, sizeof(g_idt));
    for (int i = 0; i < 32; i++) idt_set_gate(i,          isr_table[i]);
    for (int i = 0; i < 16; i++) idt_set_gate(0x20 + i,   irq_table[i]);

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (uint64_t)g_idt;
    __asm__ volatile("lidt %0" : : "m"(g_idtr));

    kprintf("[idt] installed: 32 exception + 16 IRQ vectors\n");
}
