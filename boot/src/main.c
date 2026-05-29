#include "../include/efi.h"
#include "../include/bootinfo.h"
#include "util.h"
#include "fs.h"
#include "elf.h"
#include "gop.h"
#include "mmap.h"

#define KERNEL_STACK_PAGES 16   /* 64 KiB */

static EFI_GUID gAcpi20Guid = ACPI_20_TABLE_GUID;
static EFI_GUID gAcpi10Guid = ACPI_TABLE_GUID;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) return 0;
    for (int i = 0; i < 8; i++) if (a->Data4[i] != b->Data4[i]) return 0;
    return 1;
}

static UINT64 find_rsdp(void) {
    UINT64 rsdp_v1 = 0;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *e = &gST->ConfigurationTable[i];
        if (guid_eq(&e->VendorGuid, &gAcpi20Guid)) {
            return (UINT64)(UINTN)e->VendorTable;
        }
        if (guid_eq(&e->VendorGuid, &gAcpi10Guid)) {
            rsdp_v1 = (UINT64)(UINTN)e->VendorTable;
        }
    }
    return rsdp_v1;
}

typedef void (*kernel_entry_t)(BootInfo *) __attribute__((sysv_abi));

extern "C" EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gImageHandle = ImageHandle;
    gST = SystemTable;
    gBS = SystemTable->BootServices;

    if (gST->ConOut) gST->ConOut->ClearScreen(gST->ConOut);
    put(u"[boot] VibeOS bootloader\r\n");

    /* 1. Read the kernel ELF off the ESP. */
    VOID *kernel_image = 0;
    UINTN kernel_size = 0;
    EFI_STATUS s = fs_read_file(u"\\vibeos\\kernel.elf", &kernel_image, &kernel_size);
    if (EFI_ERROR(s)) panic(u"cannot read \\vibeos\\kernel.elf", s);
    put(u"[boot] kernel.elf loaded, ");
    put_dec(kernel_size);
    put(u" bytes\r\n");

    /* 2. Parse and load PT_LOAD segments at their requested physical addresses. */
    UINT64 entry = 0, kphys_lo = 0, kphys_hi = 0;
    s = elf_load(kernel_image, kernel_size, &entry, &kphys_lo, &kphys_hi);
    if (EFI_ERROR(s)) panic(u"ELF load failed", s);
    put(u"[boot] kernel entry ");
    put_hex(entry);
    put(u" range ");
    put_hex(kphys_lo);
    put(u"..");
    put_hex(kphys_hi);
    put(u"\r\n");
    gBS->FreePool(kernel_image);

    /* 3. Allocate the kernel stack. */
    EFI_PHYSICAL_ADDRESS stack_base = 0;
    s = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                           KERNEL_STACK_PAGES, &stack_base);
    if (EFI_ERROR(s)) panic(u"stack allocation failed", s);
    UINT64 stack_top = stack_base + KERNEL_STACK_PAGES * EFI_PAGE_SIZE;

    /* 4. Allocate BootInfo as a dedicated page so it survives ExitBootServices. */
    EFI_PHYSICAL_ADDRESS bi_phys = 0;
    s = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &bi_phys);
    if (EFI_ERROR(s)) panic(u"BootInfo allocation failed", s);
    BootInfo *bi = (BootInfo *)(UINTN)bi_phys;
    bmemset(bi, 0, sizeof(*bi));
    bi->magic = BOOTINFO_MAGIC;
    bi->version = BOOTINFO_VERSION;
    bi->size = sizeof(*bi);
    bi->kernel_phys_base = kphys_lo;
    bi->kernel_phys_end = kphys_hi;
    bi->rsdp = find_rsdp();
    bi->runtime_services = (UINT64)(UINTN)gST->RuntimeServices;

    /* 5. Framebuffer. */
    s = gop_init(&bi->fb);
    if (EFI_ERROR(s)) panic(u"GOP init failed", s);
    put(u"[boot] framebuffer ");
    put_dec(bi->fb.width);
    put(u"x");
    put_dec(bi->fb.height);
    put(u" @ ");
    put_hex(bi->fb.base);
    put(u"\r\n");

    put(u"[boot] exiting boot services\r\n");

    /* 6. Final boot-services-relying step: memory map + ExitBootServices.
       After this point NO firmware print/allocate calls are valid. */
    s = mmap_exit_boot_services(&bi->mmap);
    if (EFI_ERROR(s)) {
        /* Best effort: ConOut may still work depending on firmware. */
        panic(u"ExitBootServices failed", s);
    }

    /* 7. Switch to the kernel stack and jump. SysV ABI: rdi = arg. */
    kernel_entry_t kentry = (kernel_entry_t)(UINTN)entry;
    __asm__ volatile(
        "movq %0, %%rsp\n\t"
        "xorq %%rbp, %%rbp\n\t"
        "movq %1, %%rdi\n\t"
        "callq *%2\n\t"
        :
        : "r"(stack_top), "r"(bi), "r"(kentry)
        : "memory");

    /* If the kernel returns we have nowhere to go. */
    for (;;) __asm__ volatile("cli; hlt");
}
