# x86_64-vibeos-musl cross toolchain

A cross compiler that builds binaries **for VibeOS** directly, against a VibeOS
sysroot, instead of repurposing host `musl-gcc` flags (ROADMAP §"Toolchain
integration").

VibeOS runs static and dynamically-linked `x86_64` musl binaries over the Linux
syscall ABI, so a toolchain for it is a musl sysroot wired to VibeOS's
conventions — the dynamic linker at `/lib/ld-musl-x86_64.so.1`, a `__vibeos__`
define, and `<vibeos.h>` on the default include path. Rather than rebuild GCC for
a new triple, we drive a stock host `gcc` through a specs file rooted entirely in
this sysroot (the same mechanism Debian's `musl-gcc` uses).

## Layout

- `mksysroot.sh` — assembles `sysroot/` from the host musl install and generates
  `vibeos.specs`. Re-runnable.
- `x86_64-vibeos-musl-gcc` — the cross compiler: a wrapper that runs
  `${REALGCC:-gcc}` with `vibeos.specs`. Use it exactly like `gcc`.
- `sysroot/` — generated: `usr/include` + `usr/lib` (musl headers, libc, crt),
  `lib/ld-musl-x86_64.so.1` (loader), `include/vibeos.h`. Git-ignored.

## Use

```sh
make sysroot                                   # build sysroot + specs (once)
./toolchain/x86_64-vibeos-musl-gcc -static -O2 -o hello hello.c   # static
./toolchain/x86_64-vibeos-musl-gcc          -O2 -o hello hello.c   # dynamic (PIE + ld-musl)
```

`make` builds `user/build/vibehello.elf` with this compiler as a smoke test;
`build.sh` installs it to `/bin/vibehello`, and it runs on VibeOS.

## Requirements / notes

- Needs `musl-tools` on the host (`/usr/include/x86_64-linux-musl`,
  `/usr/lib/x86_64-linux-musl`). Override with `MUSL_INC` / `MUSL_LIB`.
- `REALGCC` can point at a genuine `x86_64-*-gcc` cross compiler if you have one;
  the wrapper and sysroot still apply the VibeOS conventions.
- A from-source GCC/binutils built for the literal `x86_64-vibeos-musl` triple
  (with a config.sub patch) would drop the host-gcc dependency; the sysroot here
  is already in the layout such a build expects.
