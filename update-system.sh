#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")"

source "$PWD/build-lib.sh"

ensure_boot_image
ensure_vdisk
ensure_diskutil

step "Build kernel and userspace"
make kernel user

step "Update kernel in existing ESP image"
refresh_kernel_in_boot_image

step "Refresh core userspace in existing VibeFS volume"
sync_core_userspace

cat <<EOF

Done. Updated boot/build/vibeos.img and refreshed the core userspace files in
boot/build/vdisk.img without rebuilding package archives.
Run the OS with:

    make run
EOF