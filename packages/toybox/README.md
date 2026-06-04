# toybox (package)

[toybox](http://landley.net/toybox/) (0.8.11) — a single static binary providing
a coreutils userland (ls, cat, cp, mv, rm, mkdir, ln, grep, sed, find, …). This
is what makes the mksh prompt usable: mksh is a real shell with only a handful of
builtins, so external commands have to come from somewhere. See
[../../docs/pkgman.md](../../docs/pkgman.md) for the package model.

## Layout

```text
toybox/
    package_info.yml      # pkg create manifest (build: make in src/, stage src/build/out)
    README.md
    src/
        Makefile          # build entrypoint (fetch -> extract -> defconfig -> compile -> stage)
        .gitignore        # ignores the cached distfile, extracted tree, build/ outputs
        toybox-0.8.11.tar.gz  # cached distfile (fetched on demand; NOT committed)
```

## How it builds

Cross-compiled to a **static musl** binary (`musl-gcc`, `LDFLAGS=--static`) so it
runs unmodified on VibeOS and also on the x86_64 build host. toybox's `defconfig`
enables a few applets that include Linux UAPI headers (`linux/*`, `asm/*`) absent
from the musl sysroot; the Makefile feeds the host's kernel headers in via
`-idirafter` (musl's own libc headers still win), so the full applet set builds.

The tarball is fetched on demand and cached next to the Makefile (not committed —
the build tree is gitignored). To build by hand:

```sh
cd src && make            # produces src/build/out/bin/toybox (musl, static, stripped)
```

## How it lands in /bin

toybox is a **multicall** binary: run as `ls`, `cat`, … (selected by `argv[0]`)
it behaves as that command. After `build.sh` produces `toybox-0.8.11.pkg`, it:

1. extracts and installs the binary as `/bin/toybox`,
2. enumerates applets by running the (host-runnable, static) binary, and
3. creates a symlink farm — `/bin/ls -> /bin/toybox`, `/bin/cat -> /bin/toybox`,
   … — via `disktool-cli --symlinks`, which **skips any name already present in
   /bin** (so it never clobbers `init`, `/bin/sh`, `vsh`, `mksh`, or the test
   binaries).

If the package didn't build, `build.sh` notes that `/bin` has no coreutils and
the system still boots with the shell alone.
