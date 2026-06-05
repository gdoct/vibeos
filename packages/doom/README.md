# doom

A package-local DoomGeneric port for VibeOS. It builds a static musl `/bin/doom`
with a VibeOS-specific backend that runs as a normal GUI client under `/bin/guiwm`.
No kernel changes are required and sound is intentionally disabled.

The repository does **not** carry a committed DoomGeneric source tree. The
package Makefile downloads one pinned upstream tarball at build time, verifies
its SHA-256, unpacks it into the package work area, and adds the VibeOS backend
before compiling.

## What ships

- `/bin/doom` — DoomGeneric built against the existing userspace GUI protocol
- `share/doc/doom/README` — runtime usage and control notes
- `share/doc/doom/LICENSE` — upstream DoomGeneric license

## Runtime expectations

The package does **not** ship any IWAD data. Supply your own legal game data at
runtime, for example the shareware `doom1.wad`.

Example:

```sh
pkg extract /dist/packages/doom-1.0.0.pkg /
doom -iwad /data/doom1.wad
```

For local repo-driven testing, place a WAD under `packages/doom/local/` before
running `./build.sh`. The image builder already mirrors package source trees to
`/dist/src`, so a local file like `packages/doom/local/freedoom1.wad` is then
available inside VibeOS at `/dist/src/doom/local/freedoom1.wad`.

The binary expects the userspace window manager to be running. If `/bin/guiwm`
is not up, `/bin/doom` exits with a short error instead of touching the kernel
framebuffer directly.

## Source layout

```text
doom/
  package_info.yml
  README.md
  src/
    Makefile
    doomgeneric_vibeos.c  # VibeOS-specific backend copied into fetched upstream
    gui/                  # package-local copy of the GUI client helper/libgfx
```