# package manager `pkg`

This document is split into two parts:

- `v1` is the implementation target that should be built now.
- `future` describes the fuller package manager and ports system, but it is not required to land the first usable `pkg` tool.

The split is intentional. The roadmap currently describes `pkg` as a userspace archive tool that can list and extract POSIX ustar tarballs. The fuller package manager described below is a later layer on top of that base.

## v1: roadmap-aligned `pkg`

### scope

`v1` is a small userspace program for working with installable package archives.

- it lists the contents of a `.pkg` archive
- it extracts a `.pkg` archive into a target directory
- it can create a `.pkg` archive from a package directory that contains a valid package manifest
- it understands POSIX ustar entries for regular files, directories, and symlinks
- it does not resolve dependencies
- it does not track installed packages
- it does not uninstall packages
- it does not build ports
- it does not run maintainer scripts
- it does not need wildcard matching because package selection is explicit at this stage

This keeps `pkg` aligned with the roadmap and gives VibeOS a usable package artifact format immediately.

### package format

A `v1` package is a POSIX ustar archive with a `.pkg` extension.

- required supported entry types: regular files, directories, symlinks
- unsupported entry types should fail with a clear error
- paths in the archive should be stored as relative paths
- ownership, uid, gid, and mode bits are informational only for now
- package extraction must not allow `..` traversal or absolute-path escapes from the chosen destination
- every `v1` package must contain a `package_info.yml` manifest at the archive root
- package creation builds into a temporary staging directory, then archives `package_info.yml` plus the staged output tree

Example archive contents:

```text
package_info.yml
bin/examplepkg
lib/libexample.a
share/doc/examplepkg/README
```

### package source directory

`pkg create` operates on a package source directory that contains at least:

```text
examplepkg/
    package_info.yml
    src/
    extra/
```

The `package_info.yml` manifest must describe how to build and what to package.

Example minimal manifest:

```yaml
name: examplepkg
version: 1.0.0
build:
  command: make
  workdir: .
stage:
  from: build/out
  include:
    - bin/examplepkg
extras:
  - extra/README
```

Creation flow:

- validate that `package_info.yml` exists and has the required keys
- copy the package definition into a temporary work area
- run the manifest's build command inside that work area
- copy staged build outputs into a temporary archive root
- copy `package_info.yml` into the archive root
- optionally copy extra files named by the manifest or explicit CLI flags
- write the archive root as a `.pkg` ustar file

### commands

`v1` command surface:

```text
pkg list <archive.pkg>
pkg extract <archive.pkg> <destdir>
pkg create <pkgdir> [--output <archive.pkg>] [--include <path>]...
```

Command behavior:

- `pkg list` prints archive entries in archive order
- `pkg extract` creates directories as needed, writes files, and creates symlinks
- extraction should stop on the first hard failure and print which archive entry failed
- extracting over an existing file is allowed only if the existing entry is the same type and can be replaced safely; otherwise fail
- `pkg create` validates the manifest, copies the package definition into a temporary build area, runs the declared build command, stages the output, and writes a `.pkg` archive
- `pkg create` must always archive `package_info.yml` alongside the built output
- `pkg create --include <path>` adds extra files or directories from the package source directory into the archive root
- if `--output` is omitted, the archive name defaults to `<name>-<version>.pkg` from `package_info.yml`

Example session:

```text
$ pkg list /dist/packages/examplepkg-1.0.0.pkg
package_info.yml
bin/examplepkg
share/doc/examplepkg/README

$ pkg extract /dist/packages/examplepkg-1.0.0.pkg /
Extracting examplepkg-1.0.0.pkg...
Created /bin/examplepkg
Created /share/doc/examplepkg/README
Package extracted successfully.

$ pkg create ./packages/examplepkg --include extra/README
Building examplepkg-1.0.0...
Staging bin/examplepkg
Adding package_info.yml
Adding extra/README
Wrote examplepkg-1.0.0.pkg
```

### on-disk layout

`v1` only needs a place to store prebuilt archives inside the image.

```text
/dist/packages/
    examplepkg-1.0.0.pkg
    examplelib-1.0.0.pkg
```

`v1` can create archives locally with `pkg create`, and the image build can then copy the resulting `.pkg` files into `/dist/packages`.

### v1 implementation work

#### userspace work

- add `user/musl/pkg.c`
- add a minimal ustar reader shared with or embedded in `pkg`
- add a minimal ustar writer for `pkg create`
- implement manifest parsing for the `pkg create` build and staging keys
- implement temporary workdir setup, build execution, and staging for archive creation
- implement path sanitization during extraction
- implement path sanitization during archive creation so staged paths remain relative and archive-safe
- teach the build to copy `.pkg` archives into `/dist/packages` on the disk image
- add serial-shell test coverage for `pkg list`, `pkg extract`, and `pkg create`

#### kernel work

No new package-specific kernel ABI should be required for `v1`.

`v1` relies only on already-needed filesystem primitives:

- open/create/truncate/write/close
- mkdir
- symlink
- path resolution that follows symlinks correctly

If any of those primitives are still incomplete, finish them for general filesystem support rather than adding a package-only syscall.

## future: full package manager

This section defines the larger system that can be built after `v1` is working.

### concepts

- a `package` is an installable artifact, usually a `.pkg` archive plus metadata
- a `port` is the source-side definition used to build one or more packages
- a port points at an upstream source repository and carries the local patch set required to make that source build on VibeOS

### object model

The full system should have one source of truth per concern:

- `port.yml`: how to fetch, patch, build, and package upstream source
- `package_info.yml`: metadata embedded in or shipped with a built package
- `packages/index.yml`: repository index of available built packages
- `/var/lib/pkg/installed.yml`: installed package database on the running system
- `/var/lib/pkg/manifests/<name>.yml`: per-package installed file manifest

The package database is userspace state. It should not be encoded as per-inode kernel metadata.

### port format

Example `port.yml`:

```yaml
name: examplepkg
version: 1.0.0
source:
  repo: https://example.com/upstream/examplepkg.git
  rev: v1.0.0
patches:
  - 0001-vibeos-toolchain.patch
  - 0002-disable-unsupported-feature.patch
build:
  system: make
  args:
    - CC=x86_64-vibeos-musl-gcc
    - PREFIX=/usr
package:
  output:
    - examplepkg
```

### package metadata

Example `package_info.yml`:

```yaml
name: examplepkg
version: 1.0.0
author: anonymous <anon@some.email>
description: An example package for VibeOS
dependencies:
  - libc
  - examplelib >= 1.0.0
files:
  - /bin/examplepkg
symlinks:
  - /bin/examplepkg: /usr/bin/examplepkg
```

### repository layout

One workable repository layout for the full system:

```text
packages/
    index.yml
    examplepkg/
        port.yml
        package_info.yml
        patches/
            0001-vibeos-toolchain.patch
            0002-disable-unsupported-feature.patch
        dist/
            examplepkg-1.0.0.pkg
    examplelib/
        port.yml
        package_info.yml
        patches/
            0001-vibeos-build.patch
        dist/
            examplelib-1.0.0.pkg
```

The runtime image should expose built packages at `/dist/packages`, but the source tree may keep per-port `dist/` directories and collect them during image generation.

### future commands

Once the full package manager exists, the intended command surface is:

```text
pkg repo sync
pkg search <pattern>
pkg install <pattern>
pkg uninstall <pattern>
pkg upgrade
pkg build <pattern>
pkg info <name>
pkg list-installed
```

### wildcard semantics

Wildcard matching belongs to the full package manager, not `v1`.

- selectors use shell-style matching against package or port names
- `*` matches any sequence
- `?` matches a single character
- matches are sorted lexicographically before execution
- duplicate matches are removed before dependency resolution
- a selector that matches nothing is an error
- dependency resolution happens after selector expansion
- `pkg search <pattern>` and `pkg install <pattern>` use the same matching rules

Examples:

```text
$ pkg install 'example*'
Installing matched packages: examplelib, examplepkg...

$ pkg build 'lib*'
Building matched ports: libc, examplelib...
```

### install state model

The full package manager needs explicit userspace state.

- `installed.yml` tracks installed package name, version, install time, and reason
- each package manifest lists the files, symlinks, and directories owned by that package
- uninstall removes only files still owned by the target package
- if two packages would claim the same path, installation fails unless the file is declared as a shared directory or a managed replacement
- upgrades are modeled as install new version, then remove files no longer owned

### future implementation work

#### userspace work

- extend `pkg` from an archive tool into a repository-aware package manager
- implement parsing for `index.yml`, `package_info.yml`, `port.yml`, and installed manifests
- implement dependency resolution and version constraint checks
- implement installed-state persistence under `/var/lib/pkg`
- implement file ownership checks during install and uninstall
- implement `pkg build` to fetch sources, apply patches, run the build recipe, and emit `.pkg` archives
- implement wildcard expansion for package and port selectors

#### kernel work

The full package manager should still prefer generic filesystem support over package-specific syscalls.

Kernel work that may be needed depends on missing general features:

- robust `rename` if atomic replacement is desired during upgrades
- enough mode-bit and metadata support for packaged toolchains and libraries
- any missing directory mutation syscalls needed by install and uninstall paths
- optional file locking if concurrent package operations ever need serialization

Do not add a dedicated package-install syscall unless there is a proven filesystem limitation that cannot be solved with normal VFS operations.

## implementation order

Recommended order of work:

1. land `v1` archive listing and extraction
2. place prebuilt `.pkg` archives in `/dist/packages`
3. validate package extraction over serial in QEMU
4. add repository metadata and installed-state files
5. add dependency-aware install and uninstall
6. add ports and source builds
7. add wildcard selection, search, and upgrade flows

This order keeps the first implementation small and testable while leaving a clear path to the full system.