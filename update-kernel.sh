#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")"

source "$PWD/build-lib.sh"

step "Build kernel"
make kernel

step "Update kernel in existing ESP image"
refresh_kernel_in_boot_image

cat <<EOF

Done. Updated boot/build/vibeos.img with the current kernel/build/kernel.elf.
Run the OS with:

    make run
EOF