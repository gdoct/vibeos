#ifndef MYOS_PCI_H
#define MYOS_PCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_CLASS           0x0B
#define PCI_BAR0            0x10
#define PCI_INTERRUPT_LINE  0x3C   /* 8-bit: PIC IRQ the device is wired to */
#define PCI_INTERRUPT_PIN   0x3D   /* 8-bit: INTA#..INTD# (1..4), 0 = none */

#define PCI_CMD_IO          (1u << 0)
#define PCI_CMD_MEMORY      (1u << 1)
#define PCI_CMD_BUS_MASTER  (1u << 2)

typedef struct pci_dev {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
} pci_dev_t;

uint8_t  pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v);

/* Find the first device whose vendor matches and device id falls in
   [device_lo, device_hi]. Returns 1 if found, 0 otherwise. */
int pci_find(uint16_t vendor, uint16_t device_lo, uint16_t device_hi,
             pci_dev_t *out);

#ifdef __cplusplus
}
#endif

#endif
