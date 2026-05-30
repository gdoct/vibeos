#!/usr/bin/env bash
#
# Clean-build VibeOS and bootstrap a fresh VibeFS data disk with /bin populated
# by the diskutil-cli host tool.
#
#   1. clean build of the kernel, bootloader, userspace programs, and boot image
#   2. build the diskutil-cli (.NET) host tool
#   3. create a fresh VibeFS volume and import the userspace programs into /bin
#
# Afterwards `make run` boots QEMU with this volume attached as virtio-blk.
#
# Usage: ./build.sh [volume-size]   (default 2G; accepts K/M/G suffixes)

set -euo pipefail
cd "$(dirname "$0")"

VDISK="boot/build/vdisk.img"
VDISK_SIZE="${1:-2G}"
DISKUTIL_PROJ="interop/tools/diskutil/src/DiskTool.CLI"
DISKUTIL="$DISKUTIL_PROJ/bin/Debug/net8.0/disktool-cli"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

step "Clean build: kernel + bootloader + userspace + boot image"
make clean
rm -rf user/build            # make clean doesn't cover the userspace tree
make image                   # kernel.elf (+ user/build/{init,hello}.elf), BOOTX64.EFI, vibeos.img

step "Build diskutil-cli host tool"
dotnet build "$DISKUTIL_PROJ" -c Debug --nologo -v minimal
[ -x "$DISKUTIL" ] || { echo "error: $DISKUTIL not found after build"; exit 1; }

step "Bootstrap $VDISK ($VDISK_SIZE) with /bin"
rm -f "$VDISK"
"$DISKUTIL" --create-volume "$VDISK_SIZE" "$VDISK"
"$DISKUTIL" --diskfile "$VDISK" --mkdir /bin
"$DISKUTIL" --diskfile "$VDISK" --import user/build/init.elf  /bin/init
"$DISKUTIL" --diskfile "$VDISK" --import user/build/sh.elf    /bin/sh
"$DISKUTIL" --diskfile "$VDISK" --import user/build/hello.elf /bin/hello
[ -f user/build/mhello.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/mhello.elf /bin/mhello
[ -f user/build/ftest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/ftest.elf /bin/ftest
[ -f user/build/pipetest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/pipetest.elf /bin/pipetest
[ -f user/build/faulttest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/faulttest.elf /bin/faulttest
[ -f user/build/cputest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/cputest.elf /bin/cputest

step "Volume contents"
"$DISKUTIL" --diskfile "$VDISK" --ls /
"$DISKUTIL" --diskfile "$VDISK" --ls /bin

cat <<EOF

Done. The boot image and a fresh VibeFS volume with /bin are built.
Run the OS (mounts $VDISK as virtio-blk):

    make run
EOF
