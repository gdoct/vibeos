# MyOS top-level build.
#
# Produces:
#   boot/build/BOOTX64.EFI   — UEFI bootloader (PE32+)
#   kernel/build/kernel.elf  — ET_EXEC kernel, linked at 1 MiB phys
#   boot/build/myos.img      — FAT disk image, bootable in QEMU+OVMF
#
# Toolchain: g++ + GNU ld. Bootloader uses i386pep emulation for PE32+;
# kernel uses default elf_x86_64.
# Disk image: mtools (mformat/mmd/mcopy), no root needed.
# Test: qemu-system-x86_64 with OVMF firmware.

CC      := g++
LD      := ld
QEMU    ?= qemu-system-x86_64
OVMF    ?= /usr/share/OVMF/OVMF_CODE_4M.fd

# --- Bootloader (UEFI PE32+) ---

BOOT_CFLAGS = \
  -m64 \
  -ffreestanding \
  -fno-stack-protector \
  -fshort-wchar \
  -mno-red-zone \
  -mno-sse \
  -mno-mmx \
  -fpic \
  -nostdlib \
  -fpermissive \
  -Iboot/include \
  -O2 \
  -Wall -Wextra -Wno-unused-parameter \
  -std=c++17

# Boot is linked as an ELF DSO with a UEFI-shaped section layout, then
# objcopy converts it to PE32+ (pei-x86-64). This is the canonical recipe
# that gnu-efi/edk2 toolchains use and is what actually produces a PE
# image OVMF will load.
BOOT_LDFLAGS = \
  -nostdlib \
  -shared \
  -Bsymbolic \
  --no-undefined \
  -T boot/efi.lds \
  -e efi_main

BOOT_OBJCOPY_SECTIONS = \
  -j .text -j .sdata -j .data -j .rodata \
  -j .rel  -j .rela  -j .reloc \
  -j .dynamic

BOOT_SRCS = boot/src/main.c boot/src/util.c boot/src/fs.c \
            boot/src/elf.c  boot/src/gop.c  boot/src/mmap.c
BOOT_OBJS = $(BOOT_SRCS:boot/src/%.c=boot/build/%.o) boot/build/reloc.o

EFI = boot/build/BOOTX64.EFI
IMG = boot/build/myos.img
ESP = boot/build/esp

# --- Kernel (ET_EXEC ELF64) ---

KERNEL_CFLAGS = \
  -m64 \
  -ffreestanding \
  -fno-stack-protector \
  -fno-pic \
  -fno-pie \
  -mno-red-zone \
  -mno-sse \
  -mno-mmx \
  -mno-80387 \
  -nostdlib \
  -fno-exceptions \
  -fno-rtti \
  -Ikernel/include \
  -Iboot/include \
  -O2 \
  -Wall -Wextra -Wno-unused-parameter \
  -std=c++17

KERNEL_LDFLAGS = \
  -nostdlib \
  -static \
  -no-pie \
  -z max-page-size=0x1000 \
  -T kernel/linker.ld

KERNEL_C_SRCS = \
  kernel/src/main.c \
  kernel/src/string.c \
  kernel/src/serial.c \
  kernel/src/kio.c \
  kernel/src/panic.c \
  kernel/src/pmm.c \
  kernel/src/paging.c \
  kernel/src/kmalloc.c \
  kernel/src/device.c \
  kernel/src/fs.c \
  kernel/src/idt.c \
  kernel/src/exception.c \
  kernel/src/pic.c \
  kernel/src/irq.c \
  kernel/src/apic.c \
  kernel/src/task.c \
  kernel/src/drivers/fb.c \
  kernel/src/drivers/font.c \
  kernel/src/drivers/ramdisk.c \
  kernel/src/drivers/pci.c \
  kernel/src/drivers/virtio_blk.c \
  kernel/src/drivers/timer.c
KERNEL_S_SRCS = kernel/src/start.S kernel/src/gdt.S kernel/src/isr.S kernel/src/context_switch.S

KERNEL_C_OBJS = $(KERNEL_C_SRCS:kernel/src/%.c=kernel/build/%.o)
KERNEL_S_OBJS = $(KERNEL_S_SRCS:kernel/src/%.S=kernel/build/%.o)
KERNEL_OBJS   = $(KERNEL_S_OBJS) $(KERNEL_C_OBJS)

KERNEL_ELF = kernel/build/kernel.elf

.PHONY: all clean run image kernel
all: $(EFI) $(KERNEL_ELF)

# --- Bootloader rules ---

boot/build:
	mkdir -p boot/build

boot/build/%.o: boot/src/%.c | boot/build
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

boot/build/%.o: boot/src/%.S | boot/build
	$(CC) -c $< -o $@

boot/build/bootx64.so: $(BOOT_OBJS) boot/efi.lds
	$(LD) $(BOOT_LDFLAGS) $(BOOT_OBJS) -o $@

$(EFI): boot/build/bootx64.so
	objcopy --strip-all $(BOOT_OBJCOPY_SECTIONS) --target=pei-x86-64 --subsystem=10:2.0 $< $@

# --- Kernel rules ---

kernel/build kernel/build/drivers:
	mkdir -p $@

kernel/build/%.o: kernel/src/%.c | kernel/build kernel/build/drivers
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

kernel/build/%.o: kernel/src/%.S | kernel/build
	$(CC) -c $< -o $@

kernel: $(KERNEL_ELF)

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $@

# --- Disk image ---

image: $(IMG)

$(IMG): $(EFI) $(KERNEL_ELF)
	@mkdir -p $(ESP)/EFI/BOOT $(ESP)/myos
	cp $(EFI) $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL_ELF) $(ESP)/myos/kernel.elf
	@printf 'fs0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n' > $(ESP)/startup.nsh
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/myos
	mcopy -i $@ $(ESP)/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(ESP)/myos/kernel.elf ::/myos/kernel.elf
	mcopy -i $@ $(ESP)/startup.nsh ::/startup.nsh

VDISK = boot/build/vdisk.img

$(VDISK): | boot/build
	dd if=/dev/zero of=$@ bs=1M count=4 status=none

run: $(IMG) $(VDISK)
	$(QEMU) -machine q35 -m 256 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	  -drive format=raw,file=$(IMG) \
	  -drive if=none,id=vd0,format=raw,file=$(VDISK) \
	  -device virtio-blk-pci,drive=vd0 \
	  -net none -serial stdio

clean:
	rm -rf boot/build kernel/build
