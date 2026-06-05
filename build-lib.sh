#!/usr/bin/env bash

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="$ROOT_DIR/boot/build/vibeos.img"
VDISK="$ROOT_DIR/boot/build/vdisk.img"
DISKUTIL_PROJ="$ROOT_DIR/interop/tools/diskutil/src/DiskTool.CLI"
DISKUTIL="$DISKUTIL_PROJ/bin/Debug/net8.0/disktool-cli"
PKG_OUT_DIR="$ROOT_DIR/boot/build/packages"
MUSL_LD="/usr/lib/x86_64-linux-musl/libc.so"

CORE_USER_IMPORTS=(
	"$ROOT_DIR/user/build/vsh.elf:/bin/vsh"
	"$ROOT_DIR/user/build/hello.elf:/bin/hello"
	"$ROOT_DIR/user/build/truntest.elf:/bin/truntest"
	"$ROOT_DIR/user/build/multest.elf:/bin/multest"
	"$ROOT_DIR/user/build/mhello.elf:/bin/mhello"
	"$ROOT_DIR/user/build/ftest.elf:/bin/ftest"
	"$ROOT_DIR/user/build/pipetest.elf:/bin/pipetest"
	"$ROOT_DIR/user/build/faulttest.elf:/bin/faulttest"
	"$ROOT_DIR/user/build/cputest.elf:/bin/cputest"
	"$ROOT_DIR/user/build/sigtest.elf:/bin/sigtest"
	"$ROOT_DIR/user/build/ttytest.elf:/bin/ttytest"
	"$ROOT_DIR/user/build/nettest.elf:/bin/nettest"
	"$ROOT_DIR/user/build/wget.elf:/bin/wget"
	"$ROOT_DIR/user/build/pkg.elf:/bin/pkg"
	"$ROOT_DIR/user/build/vibehello.elf:/bin/vibehello"
	"$ROOT_DIR/user/build/abitest.elf:/bin/abitest"
	"$ROOT_DIR/user/build/threadtest.elf:/bin/threadtest"
	"$ROOT_DIR/user/build/sysconf.elf:/bin/sysconf"
	"$ROOT_DIR/user/build/heartbeat.elf:/bin/heartbeat"
	"$ROOT_DIR/user/build/guiwm.elf:/bin/guiwm"
	"$ROOT_DIR/user/build/gmandel.elf:/bin/gmandel"
	"$ROOT_DIR/user/build/gclock.elf:/bin/gclock"
	"$ROOT_DIR/user/build/gterm.elf:/bin/gterm"
	"$ROOT_DIR/user/build/guihello.elf:/bin/guihello"
	"$ROOT_DIR/user/build/guiprobe.elf:/bin/guiprobe"
)

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

die() {
	echo "error: $*" >&2
	exit 1
}

require_file() {
	local path="$1"
	local message="$2"
	[ -f "$path" ] || die "$message"
}

ensure_diskutil() {
	if [ ! -x "$DISKUTIL" ]; then
		step "Build diskutil-cli host tool"
		dotnet build "$DISKUTIL_PROJ" -c Debug --nologo -v minimal
	fi
	[ -x "$DISKUTIL" ] || die "$DISKUTIL not found after build"
}

ensure_boot_image() {
	require_file "$IMG" "boot/build/vibeos.img not found; run ./build.sh first"
}

ensure_vdisk() {
	require_file "$VDISK" "boot/build/vdisk.img not found; run ./build.sh first"
}

diskutil_cmd() {
	"$DISKUTIL" --diskfile "$VDISK" "$@"
}

diskutil_mkdir() {
	diskutil_cmd --mkdir "$1" >/dev/null 2>&1 || true
}

import_file() {
	local local_path="$1"
	local vibe_path="$2"
	require_file "$local_path" "$local_path not found"
	diskutil_cmd --import "$local_path" "$vibe_path"
}

maybe_import_file() {
	local local_path="$1"
	local vibe_path="$2"
	[ -f "$local_path" ] && diskutil_cmd --import "$local_path" "$vibe_path"
}

ensure_base_dirs() {
	diskutil_mkdir /bin
	diskutil_mkdir /etc
	diskutil_mkdir /config
	diskutil_mkdir /config/services
	diskutil_mkdir /config/logs
}

sync_config_files() {
	local svc

	ensure_base_dirs
	import_file "$ROOT_DIR/config/system.conf" /config/system.conf
	import_file "$ROOT_DIR/config/mkshrc" /etc/mkshrc
	for svc in "$ROOT_DIR"/config/services/*.yaml; do
		[ -f "$svc" ] || continue
		diskutil_cmd --import "$svc" "/config/services/$(basename "$svc")"
	done
}

sync_core_userspace() {
	local mapping src dst

	sync_config_files
	if [ -f "$ROOT_DIR/user/build/sinit.elf" ]; then
		import_file "$ROOT_DIR/user/build/sinit.elf" /bin/init
	else
		import_file "$ROOT_DIR/user/build/init.elf" /bin/init
	fi

	for mapping in "${CORE_USER_IMPORTS[@]}"; do
		IFS=: read -r src dst <<<"$mapping"
		maybe_import_file "$src" "$dst"
	done

	if [ -f "$ROOT_DIR/user/build/dynhello.elf" ] && [ -f "$MUSL_LD" ]; then
		diskutil_mkdir /lib
		import_file "$MUSL_LD" /lib/ld-musl-x86_64.so.1
		import_file "$ROOT_DIR/user/build/dynhello.elf" /bin/dynhello
	fi
}

mirror_package_source_tree() {
	local pkgdir="$1"
	local pkgroot="${pkgdir%/}"
	local name

	name="$(basename "$pkgroot")"
	diskutil_mkdir /dist
	diskutil_mkdir /dist/src
	diskutil_mkdir "/dist/src/$name"

	while IFS= read -r rel_dir; do
		[ -n "$rel_dir" ] || continue
		diskutil_cmd --mkdir "/dist/src/$name/$rel_dir" >/dev/null 2>&1 || true
	done < <(cd "$pkgroot" && find . -mindepth 1 -type d -printf '%P\n')

	while IFS= read -r rel_file; do
		[ -n "$rel_file" ] || continue
		diskutil_cmd --import "$pkgroot/$rel_file" "/dist/src/$name/$rel_file"
	done < <(cd "$pkgroot" && find . -type f -printf '%P\n')
}

build_all_package_archives() {
	local pkgtool="$ROOT_DIR/user/build/pkg.elf"
	local pkgdir name archive

	if [ ! -x "$pkgtool" ] || [ ! -d "$ROOT_DIR/packages" ]; then
		return 0
	fi

	step "Build package archives"
	rm -rf "$PKG_OUT_DIR"
	mkdir -p "$PKG_OUT_DIR"
	diskutil_mkdir /dist
	diskutil_mkdir /dist/packages

	for pkgdir in "$ROOT_DIR"/packages/*/; do
		[ -f "$pkgdir/package_info.yml" ] || continue
		name="$(basename "$pkgdir")"
		(
			cd "$PKG_OUT_DIR"
			"$pkgtool" create "$pkgdir"
		) >"$PKG_OUT_DIR/$name-build.log" 2>&1 || {
			echo "warning: pkg create failed for $name (see boot/build/packages/$name-build.log)"
			continue
		}
	done

	for archive in "$PKG_OUT_DIR"/*.pkg; do
		[ -f "$archive" ] || continue
		diskutil_cmd --import "$archive" "/dist/packages/$(basename "$archive")"
	done
}

mirror_all_package_sources() {
	local pkgdir

	if [ ! -d "$ROOT_DIR/packages" ]; then
		return 0
	fi

	for pkgdir in "$ROOT_DIR"/packages/*/; do
		[ -f "$pkgdir/package_info.yml" ] || continue
		mirror_package_source_tree "$pkgdir"
	done
}

install_mksh_from_archive() {
	local archive="$1"
	local tmpdir

	tmpdir="$(mktemp -d)"
	if tar -xf "$archive" -C "$tmpdir" bin/mksh 2>/dev/null && [ -f "$tmpdir/bin/mksh" ]; then
		chmod +x "$tmpdir/bin/mksh"
		import_file "$tmpdir/bin/mksh" /bin/mksh
		if ! diskutil_cmd --symlink /bin/mksh /bin/sh >/dev/null 2>&1; then
			diskutil_cmd --import "$tmpdir/bin/mksh" /bin/sh
		fi
		echo "default shell: /bin/sh -> /bin/mksh ($(basename "$archive"))"
	else
		echo "warning: $(basename "$archive") has no bin/mksh; falling back to /bin/vsh"
		import_file "$ROOT_DIR/user/build/vsh.elf" /bin/sh
	fi
	rm -rf "$tmpdir"
}

wire_default_shell() {
	local archive

	step "Wire default shell (/bin/sh)"
	archive="$(find "$PKG_OUT_DIR" -maxdepth 1 -type f -name 'mksh-*.pkg' | LC_ALL=C sort | tail -n 1)"
	if [ -n "$archive" ]; then
		install_mksh_from_archive "$archive"
	else
		import_file "$ROOT_DIR/user/build/vsh.elf" /bin/sh
		echo "default shell: /bin/sh = /bin/vsh (mksh package not built)"
	fi
}

install_toybox_from_archive() {
	local archive="$1"
	local tmpdir

	tmpdir="$(mktemp -d)"
	if tar -xf "$archive" -C "$tmpdir" bin/toybox 2>/dev/null && [ -f "$tmpdir/bin/toybox" ]; then
		chmod +x "$tmpdir/bin/toybox"
		import_file "$tmpdir/bin/toybox" /bin/toybox
		"$tmpdir/bin/toybox" | tr ' ' '\n' | grep -v '^$' \
			| diskutil_cmd --symlinks /bin/toybox /bin
	else
		echo "warning: $(basename "$archive") has no bin/toybox; no coreutils installed"
	fi
	rm -rf "$tmpdir"
}

install_toybox_userland() {
	local archive

	step "Install toybox userland (/bin coreutils)"
	archive="$(find "$PKG_OUT_DIR" -maxdepth 1 -type f -name 'toybox-*.pkg' | LC_ALL=C sort | tail -n 1)"
	if [ -n "$archive" ]; then
		install_toybox_from_archive "$archive"
	else
		echo "toybox package not built; /bin has no coreutils (only the shell builtins)"
	fi
}

install_doom_from_archive() {
	local archive="$1"
	local tmpdir

	tmpdir="$(mktemp -d)"
	if tar -xf "$archive" -C "$tmpdir" bin/doom 2>/dev/null && [ -f "$tmpdir/bin/doom" ]; then
		chmod +x "$tmpdir/bin/doom"
		import_file "$tmpdir/bin/doom" /bin/doom
		echo "doom test app: /bin/doom ($(basename "$archive"))"
	else
		echo "warning: $(basename "$archive") has no bin/doom; skipping direct install"
	fi
	rm -rf "$tmpdir"
}

install_doom_test_app() {
	local archive

	step "Install Doom test app (/bin/doom)"
	archive="$(find "$PKG_OUT_DIR" -maxdepth 1 -type f -name 'doom-*.pkg' | LC_ALL=C sort | tail -n 1)"
	if [ -n "$archive" ]; then
		install_doom_from_archive "$archive"
	else
		echo "doom package not built; no /bin/doom installed"
	fi
}

show_volume_contents() {
	step "Volume contents"
	diskutil_cmd --ls /
	diskutil_cmd --ls /bin
}

refresh_kernel_in_boot_image() {
	ensure_boot_image
	export MTOOLS_SKIP_CHECK=1
	mcopy -o -i "$IMG" "$ROOT_DIR/kernel/build/kernel.elf" ::/vibeos/kernel.elf
}

resolve_package_dir() {
	local spec="$1"

	if [ -d "$spec" ]; then
		(cd "$spec" && pwd)
	elif [ -d "$ROOT_DIR/packages/$spec" ]; then
		printf '%s\n' "$ROOT_DIR/packages/$spec"
	else
		die "package '$spec' not found under packages/"
	fi
}

build_one_package_archive() {
	local pkgdir="$1"
	local pkgtool="$ROOT_DIR/user/build/pkg.elf"
	local pkgname stage_dir archive copied log_file
	local -a archives

	[ -x "$pkgtool" ] || die "user/build/pkg.elf not found; run 'make user/build/pkg.elf' or 'make user' first"
	pkgname="$(basename "$pkgdir")"
	stage_dir="$(mktemp -d)"
	mkdir -p "$PKG_OUT_DIR"
	log_file="$PKG_OUT_DIR/$pkgname-build.log"

	(
		cd "$stage_dir"
		"$pkgtool" create "$pkgdir"
	) >"$log_file" 2>&1 || {
		rm -rf "$stage_dir"
		die "pkg create failed for $pkgname (see boot/build/packages/$pkgname-build.log)"
	}

	mapfile -t archives < <(find "$stage_dir" -maxdepth 1 -type f -name '*.pkg' | LC_ALL=C sort)
	if [ "${#archives[@]}" -ne 1 ]; then
		rm -rf "$stage_dir"
		die "expected exactly one archive from pkg create for $pkgname"
	fi

	copied="$PKG_OUT_DIR/$(basename "${archives[0]}")"
	cp "${archives[0]}" "$copied"
	rm -rf "$stage_dir"
	printf '%s\n' "$copied"
}

merge_package_archive_into_vdisk() {
	local archive="$1"

	diskutil_mkdir /dist
	diskutil_mkdir /dist/packages
	diskutil_cmd --import "$archive" "/dist/packages/$(basename "$archive")"
}