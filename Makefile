# VibeOS top-level build.
#
# Produces:
#   boot/build/BOOTX64.EFI   — UEFI bootloader (PE32+)
#   kernel/build/kernel.elf  — ET_EXEC kernel, higher-half (-2 GiB), loaded at 1 MiB phys
#   boot/build/vibeos.img    — FAT disk image, bootable in QEMU+OVMF
#
# Toolchain: g++ + GNU ld. Bootloader uses i386pep emulation for PE32+;
# kernel uses default elf_x86_64.
# Disk image: mtools (mformat/mmd/mcopy), no root needed.
# Test: qemu-system-x86_64 with OVMF firmware.

CC      := g++
HOSTCC  ?= gcc        # host C compiler for build-time tools (genfont)
LD      := ld
QEMU    ?= qemu-system-x86_64
OVMF    ?= /usr/share/OVMF/OVMF_CODE_4M.fd
# Display backend. Default is QEMU's own window so you can see + use the GUI;
# override with `make run QEMU_DISPLAY=-display\ none` on a headless host (the
# framebuffer still exists for `screendump` over the monitor socket).
QEMU_DISPLAY ?=

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
IMG = boot/build/vibeos.img
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
	-Igui/core/include \
	-Iboot/include \
	-O2 \
	-Wall -Wextra -Wno-unused-parameter \
	-MMD -MP \
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
	kernel/src/smp.c \
	kernel/src/task.c \
	kernel/src/percpu.c \
	kernel/src/signal.c \
	kernel/src/syscall.c \
	kernel/src/tty.c \
	kernel/src/elf64.c \
	kernel/src/random.c \
	kernel/src/csprng.c \
	kernel/src/config.c \
	kernel/src/input.c \
	kernel/src/file.c \
	kernel/src/pipe.c \
	kernel/src/synth.c \
	kernel/src/net.c \
	kernel/src/drivers/virtio_net.c \
	kernel/src/drivers/fb.c \
	kernel/src/drivers/font.c \
	kernel/src/drivers/ramdisk.c \
	kernel/src/drivers/pci.c \
	kernel/src/drivers/virtio_blk.c \
	kernel/src/drivers/virtio_rng.c \
	kernel/src/drivers/usb_uhci.c \
	kernel/src/drivers/timer.c
KERNEL_S_SRCS = kernel/src/start.S kernel/src/gdt.S kernel/src/isr.S kernel/src/context_switch.S kernel/src/usermode.S kernel/src/ap_boot.S

# GUI core (gui/core) — the kernel-side windowing lib (libdraw + libwin + libwm),
# compiled into the kernel for now. The userspace counterpart lives in gui/client.
GUI_C_SRCS = \
	gui/core/src/gui_draw.c \
	gui/core/src/gui_win.c \
	gui/core/src/gui_wm.c \
	gui/core/src/gui_logo.c

KERNEL_C_OBJS = $(KERNEL_C_SRCS:kernel/src/%.c=kernel/build/%.o)
KERNEL_S_OBJS = $(KERNEL_S_SRCS:kernel/src/%.S=kernel/build/%.o)
GUI_C_OBJS    = $(GUI_C_SRCS:gui/core/src/%.c=gui/build/%.o)
KERNEL_OBJS   = $(KERNEL_S_OBJS) $(KERNEL_C_OBJS) $(GUI_C_OBJS)

# Header-dependency tracking (-MMD): a change to a .h recompiles every .c that
# includes it. Without this, editing a struct in a header silently leaves stale
# .o files with the old layout — a brutal class of bug.
-include $(KERNEL_C_OBJS:.o=.d)
-include $(GUI_C_OBJS:.o=.d)

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
USER_SH    = user/build/sh.elf

# A real static musl binary, cross-built on the host (ROADMAP §4). This is the
# §4 proof: an unmodified Linux/musl ELF that runs under the Linux ABI. Built
# only if musl-gcc is present (apt install musl-tools).
MUSL_CC    := $(shell command -v musl-gcc 2>/dev/null)
USER_MHELLO = user/build/mhello.elf
USER_MFTEST = user/build/ftest.elf
USER_MPIPE  = user/build/pipetest.elf
USER_MFAULT = user/build/faulttest.elf
USER_MCPU   = user/build/cputest.elf
USER_MSIG   = user/build/sigtest.elf
USER_MDYN   = user/build/dynhello.elf
USER_MNET   = user/build/nettest.elf
USER_MWGET  = user/build/wget.elf
USER_MPKG   = user/build/pkg.elf
USER_VHELLO = user/build/vibehello.elf
USER_ABITEST = user/build/abitest.elf
USER_THREAD = user/build/threadtest.elf
USER_SYSCONF = user/build/sysconf.elf
USER_SINIT  = user/build/sinit.elf
USER_HEARTBEAT = user/build/heartbeat.elf

# GUI phase 2 (gui/client): a userspace window-manager server + demo clients.
# GUI_GFX is the shared drawing lib (surfaces/primitives/font/logo) linked by
# both the server and the clients; GUI_CLI is the client-side connection helper
# linked only by clients.
USER_GUIPROBE = user/build/guiprobe.elf
USER_GUIWM    = user/build/guiwm.elf
USER_GMANDEL  = user/build/gmandel.elf
USER_GCLOCK   = user/build/gclock.elf
USER_GTERM    = user/build/gterm.elf      # graphical terminal over /bin/sh
USER_GUIHELLO = user/build/guihello.elf   # skeleton example (docs/examples)
GUI_GFX = gui/client/src/libgfx.c gui/client/src/font_chicago.c gui/client/src/font_mono.c gui/client/src/gui_logo_u.c
GUI_CLI = gui/client/src/gui_client.c
GUI_INC = -Igui/client/include

# Font atlases (gui/client/src/font_*.c) are baked from TrueType at build time by
# the host genfont tool and checked in (like gui_logo_u.c). `make fonts`
# regenerates them; the normal build just compiles the committed .c. See
# gui/client/tools/genfont.c and gui/client/include/gfx_font.h.
GENFONT = gui/client/tools/genfont
.PHONY: fonts
$(GENFONT): gui/client/tools/genfont.c gui/client/tools/stb_truetype.h
	$(HOSTCC) -O2 -o $@ $< -lm
fonts: $(GENFONT)
	$(GENFONT) gui/client/assets/chicago.ttf chicago gui/client/src/font_chicago.c
	$(GENFONT) gui/client/assets/liberation_mono.ttf mono gui/client/src/font_mono.c

# VibeOS cross toolchain (ROADMAP §"Toolchain integration").
VIBEOS_CC     = ./toolchain/x86_64-vibeos-musl-gcc
SYSROOT_SPECS = toolchain/vibeos.specs

.PHONY: all clean run image kernel user sysroot
all: $(EFI) $(KERNEL_ELF) user

# Build (or rebuild) the x86_64-vibeos-musl sysroot + specs from host musl.
sysroot: $(SYSROOT_SPECS)
$(SYSROOT_SPECS): toolchain/mksysroot.sh
ifeq ($(MUSL_CC),)
	@echo "warning: musl-tools not found; skipping VibeOS sysroot"
else
	./toolchain/mksysroot.sh
endif

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

# GUI core sources live under gui/core/src but compile with the kernel flags and
# link into the kernel — same toolchain, just a separate source tree.
gui/build:
	mkdir -p $@

gui/build/%.o: gui/core/src/%.c | gui/build
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

kernel: $(KERNEL_ELF)

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $@

# --- Userspace programs ---
# Built as standalone ELFs and installed onto the VibeFS data disk by the host
# tool (build.sh / diskutil-cli) — the kernel loads /bin/init from disk at boot,
# so nothing is embedded in the kernel image.

user: $(USER_INIT) $(USER_HELLO) $(USER_SH) $(USER_MHELLO) $(USER_MFTEST) $(USER_MPIPE) $(USER_MFAULT) $(USER_MCPU) $(USER_MSIG) $(USER_MDYN) $(USER_MNET) $(USER_MWGET) $(USER_MPKG) $(USER_VHELLO) $(USER_ABITEST) $(USER_THREAD) $(USER_SYSCONF) $(USER_SINIT) $(USER_HEARTBEAT) $(USER_GUIPROBE) $(USER_GUIWM) $(USER_GMANDEL) $(USER_GCLOCK) $(USER_GTERM) $(USER_GUIHELLO)

# Static musl builds (host cross-compile). Skipped with a note if musl-gcc is
# absent, so the rest of the build still works.
$(USER_MHELLO): user/musl/hello.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found (apt install musl-tools); skipping $(USER_MHELLO)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_MFTEST): user/musl/ftest.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found (apt install musl-tools); skipping $(USER_MFTEST)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_MPIPE): user/musl/pipetest.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found (apt install musl-tools); skipping $(USER_MPIPE)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_MFAULT): user/musl/faulttest.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found (apt install musl-tools); skipping $(USER_MFAULT)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_MCPU): user/musl/cputest.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found (apt install musl-tools); skipping $(USER_MCPU)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_MSIG): user/musl/sigtest.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_MSIG)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

# Dynamically-linked musl PIE (NOT -static): exercises the §4 dynamic loader.
$(USER_MDYN): user/musl/dynhello.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_MDYN)"
else
	$(MUSL_CC) -O2 -o $@ $<
endif

$(USER_MNET): user/musl/nettest.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_MNET)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_GUIPROBE): user/musl/guiprobe.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_GUIPROBE)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_GUIWM): user/musl/guiwm.c $(GUI_GFX) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_GUIWM)"
else
	$(MUSL_CC) -static -no-pie -O2 $(GUI_INC) -o $@ $< $(GUI_GFX)
endif

$(USER_GMANDEL): user/musl/gmandel.c $(GUI_GFX) $(GUI_CLI) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_GMANDEL)"
else
	$(MUSL_CC) -static -no-pie -O2 $(GUI_INC) -o $@ $< $(GUI_GFX) $(GUI_CLI)
endif

$(USER_GCLOCK): user/musl/gclock.c $(GUI_GFX) $(GUI_CLI) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_GCLOCK)"
else
	$(MUSL_CC) -static -no-pie -O2 $(GUI_INC) -o $@ $< $(GUI_GFX) $(GUI_CLI)
endif

$(USER_GTERM): user/musl/gterm.c $(GUI_GFX) $(GUI_CLI) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_GTERM)"
else
	$(MUSL_CC) -static -no-pie -O2 $(GUI_INC) -o $@ $< $(GUI_GFX) $(GUI_CLI)
endif

# The skeleton example app lives under docs/examples (it doubles as a runnable
# launcher demo). Builds exactly like any other GUI client.
$(USER_GUIHELLO): docs/examples/guihello.c $(GUI_GFX) $(GUI_CLI) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_GUIHELLO)"
else
	$(MUSL_CC) -static -no-pie -O2 $(GUI_INC) -o $@ $< $(GUI_GFX) $(GUI_CLI)
endif

$(USER_MWGET): user/musl/wget.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_MWGET)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_MPKG): user/musl/pkg.c | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-gcc not found; skipping $(USER_MPKG)"
else
	$(MUSL_CC) -static -no-pie -O2 -o $@ $<
endif

# Built with the VibeOS *cross* compiler against the sysroot, not host musl-gcc.
$(USER_VHELLO): user/musl/vibehello.c $(SYSROOT_SPECS) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-tools not found; skipping $(USER_VHELLO)"
else
	$(VIBEOS_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_ABITEST): user/musl/abitest.c $(SYSROOT_SPECS) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-tools not found; skipping $(USER_ABITEST)"
else
	$(VIBEOS_CC) -static -no-pie -O2 -o $@ $<
endif

# Multithreaded (pthreads) — exercises clone/futex. Static so it's self-contained.
$(USER_THREAD): user/musl/threadtest.c $(SYSROOT_SPECS) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-tools not found; skipping $(USER_THREAD)"
else
	$(VIBEOS_CC) -static -no-pie -O2 -pthread -o $@ $<
endif

$(USER_SYSCONF): user/musl/sysconf.c $(SYSROOT_SPECS) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-tools not found; skipping $(USER_SYSCONF)"
else
	$(VIBEOS_CC) -static -no-pie -O2 -o $@ $<
endif

# Service-managed init (musl: dirent/stdio). Installed as /bin/init by build.sh
# when present; the freestanding user/init.c is the fallback.
$(USER_SINIT): user/musl/sinit.c $(SYSROOT_SPECS) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-tools not found; skipping $(USER_SINIT)"
else
	$(VIBEOS_CC) -static -no-pie -O2 -o $@ $<
endif

$(USER_HEARTBEAT): user/musl/heartbeat.c $(SYSROOT_SPECS) | user/build
ifeq ($(MUSL_CC),)
	@echo "warning: musl-tools not found; skipping $(USER_HEARTBEAT)"
else
	$(VIBEOS_CC) -static -no-pie -O2 -o $@ $<
endif

user/build:
	mkdir -p user/build

user/build/crt0.o: user/crt0.S | user/build
	$(CC) -m64 -c $< -o $@

user/build/init.o: user/init.c | user/build
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/build/hello.o: user/hello.c | user/build
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/build/sh.o: user/sh.c | user/build
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_INIT): user/build/crt0.o user/build/init.o user/user.ld
	$(LD) $(USER_LDFLAGS) user/build/crt0.o user/build/init.o -o $@

$(USER_HELLO): user/build/crt0.o user/build/hello.o user/user.ld
	$(LD) $(USER_LDFLAGS) user/build/crt0.o user/build/hello.o -o $@

$(USER_SH): user/build/crt0.o user/build/sh.o user/user.ld
	$(LD) $(USER_LDFLAGS) user/build/crt0.o user/build/sh.o -o $@

# --- Disk image ---

image: user $(IMG)

$(IMG): $(EFI) $(KERNEL_ELF)
	@mkdir -p $(ESP)/EFI/BOOT $(ESP)/vibeos
	cp $(EFI) $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL_ELF) $(ESP)/vibeos/kernel.elf
	@printf 'fs0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n' > $(ESP)/startup.nsh
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/vibeos
	mcopy -i $@ $(ESP)/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(ESP)/vibeos/kernel.elf ::/vibeos/kernel.elf
	mcopy -i $@ $(ESP)/startup.nsh ::/startup.nsh

VDISK = boot/build/vdisk.img

# Sparse 2 GiB virtio disk for the VibeFS volume. Sparse, so it costs almost
# nothing on the host until blocks are actually written. The fs self-test's
# 5 GiB-offset file is sparse (a handful of low-numbered physical blocks), so
# it still fits comfortably.
VDISK_BYTES = 2147483648      # 2 GiB
$(VDISK): | boot/build
	truncate -s $(VDISK_BYTES) $@

run: $(IMG) $(VDISK)
	$(QEMU) -machine q35 -m 256 -smp 4 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	  -drive format=raw,file=$(IMG) \
	  -drive if=none,id=vd0,format=raw,file=$(VDISK) \
	  -device virtio-blk-pci,drive=vd0 \
	  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	  -device virtio-rng-pci \
	  -device piix3-usb-uhci,id=uhci \
	  -device usb-kbd,bus=uhci.0,port=1 -device usb-tablet,bus=uhci.0,port=2 \
	  -vga none -device VGA,edid=on,xres=1280,yres=960 \
	  -monitor unix:/tmp/vibeos-mon.sock,server,nowait \
	  $(QEMU_DISPLAY) \
	  -serial stdio

clean:
	rm -rf boot/build kernel/build gui/build user/build
