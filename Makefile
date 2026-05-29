# MyOS top-level build.
#
# Produces:
#   boot/build/BOOTX64.EFI   — UEFI bootloader (PE32+)
#   kernel/build/kernel.elf  — ET_EXEC kernel, higher-half (-2 GiB), loaded at 1 MiB phys
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
  -mcmodel=kernel \
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
  kernel/src/tss.c \
  kernel/src/syscall.c \
  kernel/src/elf64.c \
  kernel/src/drivers/fb.c \
  kernel/src/drivers/font.c \
  kernel/src/drivers/ramdisk.c \
  kernel/src/drivers/pci.c \
  kernel/src/drivers/virtio_blk.c \
  kernel/src/drivers/timer.c
KERNEL_S_SRCS = kernel/src/start.S kernel/src/gdt.S kernel/src/isr.S kernel/src/context_switch.S kernel/src/usermode.S

KERNEL_C_OBJS = $(KERNEL_C_SRCS:kernel/src/%.c=kernel/build/%.o)
KERNEL_S_OBJS = $(KERNEL_S_SRCS:kernel/src/%.S=kernel/build/%.o)
# *_blob.o embed userspace program ELFs; built via objcopy (see below).
USER_BLOB_OBJ  = kernel/build/user_blob.o
HELLO_BLOB_OBJ = kernel/build/hello_blob.o
KERNEL_OBJS    = $(KERNEL_S_OBJS) $(KERNEL_C_OBJS) $(USER_BLOB_OBJ) $(HELLO_BLOB_OBJ)

KERNEL_ELF = kernel/build/kernel.elf

# --- Userspace (ROADMAP §3) ---

USER_CFLAGS = \
  -m64 -ffreestanding -fno-pic -fno-pie \
  -mno-red-zone -mno-sse -mno-mmx \
  -nostdlib -fno-exceptions -fno-rtti -fno-stack-protector \
  -O2 -Wall -Wextra -Wno-unused-parameter -std=c++17
USER_LDFLAGS = -nostdlib -static -no-pie -T user/user.ld

USER_INIT  = user/build/init.elf
USER_HELLO = user/build/hello.elf

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

# --- Userspace init + blob embedding ---

user/build:
	mkdir -p user/build

user/build/crt0.o: user/crt0.S | user/build
	$(CC) -m64 -c $< -o $@

user/build/init.o: user/init.c | user/build
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/build/hello.o: user/hello.c | user/build
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_INIT): user/build/crt0.o user/build/init.o user/user.ld
	$(LD) $(USER_LDFLAGS) user/build/crt0.o user/build/init.o -o $@

$(USER_HELLO): user/build/crt0.o user/build/hello.o user/user.ld
	$(LD) $(USER_LDFLAGS) user/build/crt0.o user/build/hello.o -o $@

# Wrap each user ELF as an object the kernel links in, exposing
# <name>_elf_start / <name>_elf_end around its bytes.
$(USER_BLOB_OBJ): $(USER_INIT) | kernel/build
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  --redefine-sym _binary_user_build_init_elf_start=init_elf_start \
	  --redefine-sym _binary_user_build_init_elf_end=init_elf_end \
	  --redefine-sym _binary_user_build_init_elf_size=init_elf_size \
	  $< $@

$(HELLO_BLOB_OBJ): $(USER_HELLO) | kernel/build
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  --redefine-sym _binary_user_build_hello_elf_start=hello_elf_start \
	  --redefine-sym _binary_user_build_hello_elf_end=hello_elf_end \
	  --redefine-sym _binary_user_build_hello_elf_size=hello_elf_size \
	  $< $@

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

# Sparse 8 GiB virtio disk: large enough to host files past the old 4 MiB /
# 128 MiB ceilings (and, structurally, multi-GiB files). Sparse, so it costs
# almost nothing on the host until blocks are actually written.
VDISK_BYTES = 8589934592      # 8 GiB
$(VDISK): | boot/build
	truncate -s $(VDISK_BYTES) $@

run: $(IMG) $(VDISK)
	$(QEMU) -machine q35 -m 256 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	  -drive format=raw,file=$(IMG) \
	  -drive if=none,id=vd0,format=raw,file=$(VDISK) \
	  -device virtio-blk-pci,drive=vd0 \
	  -net none -serial stdio

clean:
	rm -rf boot/build kernel/build
