#ifndef MYOS_IDT_H
#define MYOS_IDT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel segment selectors set up by gdt_init(). */
#define KERNEL_CS  0x08
#define KERNEL_DS  0x10

void gdt_init(void);   /* installs our own GDT, reloads segments */
void idt_init(void);   /* installs IDT with exception handlers for vectors 0-31 */

#ifdef __cplusplus
}
#endif

#endif
