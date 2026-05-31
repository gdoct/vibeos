#!/usr/bin/env bash
#
# Build the x86_64-vibeos-musl sysroot (ROADMAP §"Toolchain integration").
#
# VibeOS runs static and dynamically-linked x86_64 musl binaries over the Linux
# syscall ABI, so a "cross toolchain" for it is a musl sysroot wired to VibeOS's
# conventions: the dynamic linker lives at /lib/ld-musl-x86_64.so.1 (where
# build.sh installs it), the target defines __vibeos__, and <vibeos.h> is on the
# default include path. We assemble that sysroot from the host musl install and
# generate a specs file so a stock host gcc, invoked through the
# x86_64-vibeos-musl-gcc wrapper, compiles and links against *this* sysroot
# rather than ad-hoc host paths.
#
# Re-runnable; rebuilds toolchain/sysroot and toolchain/vibeos.specs in place.

set -euo pipefail
cd "$(dirname "$0")"
TC="$PWD"
SYSROOT="$TC/sysroot"

# Host musl locations (Debian/Ubuntu musl-tools layout).
MUSL_INC="${MUSL_INC:-/usr/include/x86_64-linux-musl}"
MUSL_LIB="${MUSL_LIB:-/usr/lib/x86_64-linux-musl}"

[ -d "$MUSL_INC" ] || { echo "error: musl headers not found at $MUSL_INC (apt install musl-tools)"; exit 1; }
[ -d "$MUSL_LIB" ] || { echo "error: musl libs not found at $MUSL_LIB"; exit 1; }

echo "==> assembling sysroot at $SYSROOT"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib" "$SYSROOT/lib" \
         "$SYSROOT/include" "$SYSROOT/bin"

# musl headers + libs + crt objects.
cp -a "$MUSL_INC/." "$SYSROOT/usr/include/"
cp -a "$MUSL_LIB/." "$SYSROOT/usr/lib/"

# The dynamic linker, at the VibeOS path. The host musl libc.so doubles as the
# loader (same as build.sh installs into the VibeFS image).
if [ -f "$MUSL_LIB/libc.so" ]; then
    cp -a "$MUSL_LIB/libc.so" "$SYSROOT/lib/ld-musl-x86_64.so.1"
fi

# VibeOS target header — lets programs detect and adapt to the target.
cat > "$SYSROOT/include/vibeos.h" <<'EOF'
#ifndef _VIBEOS_H
#define _VIBEOS_H
/* Marker header for the x86_64-vibeos-musl target. VibeOS follows the Linux
   x86_64 syscall ABI, so the standard musl headers apply; this just identifies
   the target and pins the runtime layout the loader expects. */
#define VIBEOS 1
#define VIBEOS_DYNAMIC_LINKER "/lib/ld-musl-x86_64.so.1"
#endif
EOF

# Generate the specs file: musl-gcc's, but every host path rewritten into the
# sysroot, plus -isystem for <vibeos.h> and a -D__vibeos__ define.
echo "==> generating vibeos.specs"
cat > "$TC/vibeos.specs" <<EOF
%rename cpp_options old_cpp_options

*cpp_options:
-nostdinc -isystem $SYSROOT/usr/include -isystem $SYSROOT/include -isystem include%s -D__vibeos__ %(old_cpp_options)

*cc1:
%(cc1_cpu) -nostdinc -isystem $SYSROOT/usr/include -isystem $SYSROOT/include -isystem include%s -D__vibeos__

*link_libgcc:
-L$SYSROOT/usr/lib -L .%s

*libgcc:
libgcc.a%s %:if-exists(libgcc_eh.a%s)

*startfile:
%{shared:;static-pie:$SYSROOT/usr/lib/rcrt1.o; :$SYSROOT/usr/lib/Scrt1.o} $SYSROOT/usr/lib/crti.o crtbeginS.o%s

*endfile:
crtendS.o%s $SYSROOT/usr/lib/crtn.o

*link:
-dynamic-linker /lib/ld-musl-x86_64.so.1 -nostdlib %{shared:-shared} %{static:-static} %{static-pie:-static -pie --no-dynamic-linker} %{rdynamic:-export-dynamic}
EOF

echo "==> done. Cross compiler: $TC/x86_64-vibeos-musl-gcc"
echo "    sysroot: $SYSROOT"
