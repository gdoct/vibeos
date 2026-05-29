#ifndef VIBEOS_BOOTINFO_H
#define VIBEOS_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC   0x544F4F4253454249ULL  /* "VIBEBOOT" little-endian */
#define BOOTINFO_VERSION 1

typedef enum {
    FB_FORMAT_BGR8 = 0,  /* 32-bit pixels, 0x00RRGGBB in memory */
    FB_FORMAT_RGB8 = 1,
} FramebufferFormat;

typedef struct {
    uint64_t base;         /* physical address of the framebuffer */
    uint64_t size;         /* total size in bytes */
    uint32_t width;        /* in pixels */
    uint32_t height;       /* in pixels */
    uint32_t pitch;        /* bytes per scanline (may exceed width*4) */
    uint32_t format;       /* FramebufferFormat */
} FramebufferInfo;

/*
 * Mirrors EFI_MEMORY_DESCRIPTOR layout exactly. We pass the raw UEFI
 * memory map through to the kernel so it can build its own allocator.
 */
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;    /* 4 KiB pages */
    uint64_t attribute;
} MemoryDescriptor;

typedef struct {
    uint64_t buffer;       /* physical address of descriptor array */
    uint64_t size;         /* total bytes valid in buffer */
    uint64_t desc_size;    /* size of each descriptor (UEFI may grow it) */
    uint32_t desc_version;
    uint32_t pad;
} MemoryMap;

typedef struct {
    uint64_t magic;            /* BOOTINFO_MAGIC */
    uint32_t version;          /* BOOTINFO_VERSION */
    uint32_t size;             /* sizeof(BootInfo) */

    FramebufferInfo fb;
    MemoryMap mmap;

    uint64_t rsdp;             /* ACPI RSDP physical address, 0 if absent */
    uint64_t runtime_services; /* EFI_RUNTIME_SERVICES* */

    uint64_t kernel_phys_base; /* lowest physical page used by kernel image */
    uint64_t kernel_phys_end;  /* exclusive upper bound */
} BootInfo;

#endif
