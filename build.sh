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

VDISK_SIZE="${1:-2G}"

source "$PWD/build-lib.sh"

step "Clean build: kernel + bootloader + userspace + boot image"
make clean
rm -rf user/build            # make clean doesn't cover the userspace tree
make image                   # kernel.elf (+ user/build/{init,hello}.elf), BOOTX64.EFI, vibeos.img

ensure_diskutil

step "Bootstrap $VDISK ($VDISK_SIZE) with /bin"
rm -f "$VDISK"
"$DISKUTIL" --create-volume "$VDISK_SIZE" "$VDISK"
sync_core_userspace
build_all_package_archives
mirror_all_package_sources
wire_default_shell
install_toybox_userland
install_doom_test_app
show_volume_contents

cat <<EOF

Done. The boot image and a fresh VibeFS volume with /bin are built.
Run the OS (mounts $VDISK as virtio-blk):

    make run
EOF
