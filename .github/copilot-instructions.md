# Copilot Instructions

Use [docs/ROADMAP.md](docs/ROADMAP.md) as the source of truth for shipped
features, backlog order, and design decisions.

For build and image update workflows in this repo:

- `make` builds the bootloader, kernel, and userspace binaries.
- `./build.sh [size]` performs a clean rebuild and creates a fresh VibeFS disk
  image.
- `./update-kernel.sh` rebuilds `kernel.elf` and replaces it in the existing
  `boot/build/vibeos.img` ESP image.
- `./update-system.sh` rebuilds the kernel plus userspace and refreshes the
  existing `boot/build/vibeos.img` and `boot/build/vdisk.img` images without
  rebuilding packages.
- `./merge-package.sh <pkg>` builds one package and merges its archive into the
  existing `boot/build/vdisk.img` image.
- `make run` is the end-to-end validation path; prefer serial-log verification
  in QEMU over compile-only checks.

Conventions to preserve:

- Keep the codebase in C-style C++17 with no RTTI, exceptions, or STL.
- Preserve the higher-half memory model; do not assume low-half identity maps
  after boot.
- Match Linux syscall numbers and ABI behavior so musl binaries keep running
  unmodified.