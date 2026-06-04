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
"$DISKUTIL" --diskfile "$VDISK" --mkdir /config
"$DISKUTIL" --diskfile "$VDISK" --mkdir /config/services
"$DISKUTIL" --diskfile "$VDISK" --mkdir /config/logs
"$DISKUTIL" --diskfile "$VDISK" --import config/system.conf /config/system.conf
# Service definitions (ROADMAP: service-managed init) — one discoverable YAML per
# service, read by the init at boot.
for svc in config/services/*.yaml; do
  [ -f "$svc" ] && "$DISKUTIL" --diskfile "$VDISK" --import "$svc" "/config/services/$(basename "$svc")"
done
# /bin/init is the service-managed init (sinit) when built; else the freestanding
# fallback (user/init.c).
if [ -f user/build/sinit.elf ]; then
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/sinit.elf /bin/init
else
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/init.elf  /bin/init
fi
"$DISKUTIL" --diskfile "$VDISK" --import user/build/sh.elf    /bin/sh
"$DISKUTIL" --diskfile "$VDISK" --import user/build/hello.elf /bin/hello
[ -f user/build/truntest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/truntest.elf /bin/truntest
[ -f user/build/multest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/multest.elf /bin/multest
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
[ -f user/build/sigtest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/sigtest.elf /bin/sigtest
[ -f user/build/ttytest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/ttytest.elf /bin/ttytest
[ -f user/build/nettest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/nettest.elf /bin/nettest
[ -f user/build/wget.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/wget.elf /bin/wget
[ -f user/build/pkg.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/pkg.elf /bin/pkg
[ -f user/build/vibehello.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/vibehello.elf /bin/vibehello
[ -f user/build/abitest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/abitest.elf /bin/abitest
[ -f user/build/threadtest.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/threadtest.elf /bin/threadtest
[ -f user/build/sysconf.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/sysconf.elf /bin/sysconf
[ -f user/build/heartbeat.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/heartbeat.elf /bin/heartbeat
# GUI phase 2 (gui/client): the userspace window manager + demo clients.
[ -f user/build/guiwm.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/guiwm.elf /bin/guiwm
[ -f user/build/gmandel.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/gmandel.elf /bin/gmandel
[ -f user/build/gclock.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/gclock.elf /bin/gclock
[ -f user/build/gterm.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/gterm.elf /bin/gterm
[ -f user/build/guihello.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/guihello.elf /bin/guihello
[ -f user/build/guiprobe.elf ] && \
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/guiprobe.elf /bin/guiprobe

# Dynamic linking (ROADMAP §4): ship the musl dynamic linker as
# /lib/ld-musl-x86_64.so.1 (the host's musl libc.so doubles as the loader) and a
# dynamically-linked test program. The kernel reads /bin/dynhello's PT_INTERP,
# maps the linker, and lets it relocate + run the program.
MUSL_LD="/usr/lib/x86_64-linux-musl/libc.so"
if [ -f user/build/dynhello.elf ] && [ -f "$MUSL_LD" ]; then
  "$DISKUTIL" --diskfile "$VDISK" --mkdir /lib
  "$DISKUTIL" --diskfile "$VDISK" --import "$MUSL_LD" /lib/ld-musl-x86_64.so.1
  "$DISKUTIL" --diskfile "$VDISK" --import user/build/dynhello.elf /bin/dynhello
fi

step "Volume contents"
"$DISKUTIL" --diskfile "$VDISK" --ls /
"$DISKUTIL" --diskfile "$VDISK" --ls /bin

cat <<EOF

Done. The boot image and a fresh VibeFS volume with /bin are built.
Run the OS (mounts $VDISK as virtio-blk):

    make run
EOF
