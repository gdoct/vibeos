#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")"

source "$PWD/build-lib.sh"

if [ "$#" -ne 1 ]; then
	echo "usage: $0 <package-name-or-path>" >&2
	exit 1
fi

ensure_vdisk
ensure_diskutil

PKGDIR="$(resolve_package_dir "$1")"
PKGNAME="$(basename "$PKGDIR")"

step "Build pkg tool"
make user/build/pkg.elf

step "Build $PKGNAME package archive"
ARCHIVE="$(build_one_package_archive "$PKGDIR")"

step "Merge $PKGNAME package into existing VibeFS volume"
merge_package_archive_into_vdisk "$ARCHIVE"
mirror_package_source_tree "$PKGDIR"

case "$PKGNAME" in
	mksh)
		step "Install mksh into /bin"
		install_mksh_from_archive "$ARCHIVE"
		;;
	toybox)
		step "Install toybox userland into /bin"
		install_toybox_from_archive "$ARCHIVE"
		;;
	esac

cat <<EOF

Done. Merged $(basename "$ARCHIVE") into boot/build/vdisk.img and refreshed
/dist/src/$PKGNAME from the local package tree.
Run the OS with:

    make run
EOF