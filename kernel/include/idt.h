#ifndef VIBEOS_IDT_H
#define VIBEOS_IDT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Segment selectors set up by gdt_init(). The user selectors carry RPL 3.
   Their GDT order (udata before ucode) is fixed by what SYSRET expects:
   it loads SS from STAR.SYSRET_base+8 and CS from +16 (see syscall_init). */
#define KERNEL_CS  0x08
#define KERNEL_DS  0x10
#define USER_DS    (0x18 | 3)   /* GDT[3], RPL 3 */
#define USER_CS    (0x20 | 3)   /* GDT[4], RPL 3 */
#define TSS_SEL    0x28         /* GDT[5..6] (16-byte system descriptor) */

void gdt_init(void);   /* installs our own GDT, reloads segments */
void idt_init(void);   /* installs IDT with exception handlers for vectors 0-31 */

/* Point an arbitrary IDT vector at a raw handler (e.g. the LAPIC spurious
   stub). Interrupt gate, kernel CS, DPL 0. */
void idt_set_vector(int vec, void *handler);

#ifdef __cplusplus
}
#endif

#endif
