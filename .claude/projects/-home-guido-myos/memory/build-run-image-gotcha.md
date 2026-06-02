---
name: build-run-image-gotcha
description: Bootable image needs `make image`, not just `make`; bare `make` can skip the kernel relink
metadata:
  type: project
---

When boot-testing kernel changes in QEMU, `make` / `make all` build `kernel/build/kernel.elf` but do **not** refresh the bootable ESP image `boot/build/vibeos.img` — that is a separate `make image` target (`$(IMG): $(EFI) $(KERNEL_ELF)`, copies the elf into the ESP). QEMU boots `vibeos.img`, so running qemu after only `make` boots a **stale kernel**.

Also observed: a bare `make` sometimes reported only "`kernel/build/main.o` is up to date" and did **not** relink `kernel.elf` even when other objects (e.g. `task.o`) were newer — so the elf silently went stale. `make all` reliably recompiles changed TUs and relinks.

**Always build with `make all && make image` before launching QEMU**, and sanity-check `ls -la boot/build/vibeos.img kernel/build/kernel.elf` timestamps. This cost real debugging time (a feature looked broken because the test booted an old image). Related: [[smp-userspace-multicore]].
