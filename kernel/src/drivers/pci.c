#include "kernel.h"
#include "io.h"
#include "pci.h"

/* Type 1 configuration access via the legacy IO ports.
   CONFIG_ADDRESS layout:
     bit 31    enable
     bit 23:16 bus
     bit 15:11 device
     bit 10:8  function
     bit 7:2   register (must be 32-bit aligned)
     bit 1:0   zero
*/
#define CFG_ADDR  0xCF8
#define CFG_DATA  0xCFC

static inline uint32_t cfg_address(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (1u << 31)
         | ((uint32_t)bus << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(fn  & 0x07) << 8)
         | ((uint32_t)(off & 0xFC));
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(CFG_ADDR, cfg_address(bus, dev, fn, off));
    return inl(CFG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(CFG_ADDR, cfg_address(bus, dev, fn, off));
    outl(CFG_DATA, v);
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, fn, off & ~3);
    return (uint8_t)(v >> ((off & 3) * 8));
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, fn, off & ~3);
    return (uint16_t)(v >> ((off & 2) * 8));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v) {
    uint8_t aligned = off & ~3;
    uint32_t cur = pci_read32(bus, dev, fn, aligned);
    int shift = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
    pci_write32(bus, dev, fn, aligned, cur);
}

int pci_find(uint16_t vendor, uint16_t device_lo, uint16_t device_hi,
             pci_dev_t *out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t vd = pci_read32((uint8_t)bus, dev, fn, 0);
                uint16_t ven = (uint16_t)(vd & 0xFFFF);
                if (ven == 0xFFFF) continue;
                if (ven != vendor) continue;
                uint16_t did = (uint16_t)(vd >> 16);
                if (did < device_lo || did > device_hi) continue;
                out->bus    = (uint8_t)bus;
                out->dev    = dev;
                out->fn     = fn;
                out->vendor = ven;
                out->device = did;
                return 1;
            }
        }
    }
    return 0;
}

int pci_find_subsys(uint16_t vendor, uint16_t subsys, pci_dev_t *out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t vd = pci_read32((uint8_t)bus, dev, fn, 0);
                uint16_t ven = (uint16_t)(vd & 0xFFFF);
                if (ven == 0xFFFF || ven != vendor) continue;
                if (pci_read16((uint8_t)bus, dev, fn, PCI_SUBSYSTEM_ID) != subsys) continue;
                out->bus = (uint8_t)bus; out->dev = dev; out->fn = fn;
                out->vendor = ven;
                out->device = (uint16_t)(vd >> 16);
                return 1;
            }
        }
    }
    return 0;
}
