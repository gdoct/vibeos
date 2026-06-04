# mksh (package)

The [MirBSD Korn Shell](http://www.mirbsd.org/mksh.htm) (R59c), ported to VibeOS
as the **default shell**. See [../../docs/pkgman.md](../../docs/pkgman.md) for the
`v1` package model and [package_info.yml](package_info.yml) for the manifest.

## Layout

```text
mksh/
    package_info.yml      # pkg create manifest (build: make in src/, stage src/build/out)
    README.md
    src/
        Makefile          # the build entrypoint (fetch -> extract -> compile -> stage)
        .gitignore        # ignores the cached distfile, extracted tree, build/ outputs
        mksh-R59c.tgz     # cached distfile (fetched on demand; NOT committed)
```

The upstream tarball is **fetched on demand** by the Makefile, which pins the
source URL(s) and SHA-256 and caches the download next to itself. It is not
committed to git — the repo's top-level `.gitignore` excludes `*.tgz`. A build
host therefore needs network access the first time (the canonical MirBSD mirror,
https with an http fallback); subsequent builds reuse the cached tarball.

## How it builds

mksh has no Makefile of its own — it configures and compiles via its own
`Build.sh`, which compiles small probe programs and *runs* them to detect target
features. We build a **static** binary with `musl-gcc`; static musl binaries also
run on the x86_64 build host, so the probes execute and report the target
(`x86_64-linux-musl`) characteristics correctly. The result is a static ELF with
no interpreter that runs unmodified on VibeOS (Linux syscall ABI).

`pkg create packages/mksh` (run by `build.sh`) copies this directory into a work
area, runs `make` in `src/` (extract + compile + strip), and archives the staged
`src/build/out/bin/mksh` into `mksh-R59c.pkg` as `bin/mksh`.

To build it by hand:

```sh
cd src && make            # produces src/build/out/bin/mksh (musl, static, stripped)
```

## How it becomes the default shell

`init` execs `/bin/sh`. After `build.sh` produces `mksh-R59c.pkg`, it:

1. extracts `bin/mksh` from that archive (a plain ustar `.pkg`),
2. imports it as `/bin/mksh`, and
3. creates the symlink `/bin/sh -> /bin/mksh` (via `disktool-cli --symlink
   <target> <linkpath>`).

If the package didn't build (e.g. no toolchain or no network and no vendored
tarball), `build.sh` falls back to importing `/bin/vsh`
([../../user/vsh.c](../../user/vsh.c)) as `/bin/sh` so the system still boots.
This mirrors the existing `sinit`/`init` fallback pattern.
