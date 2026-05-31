/*
 * vibehello — built by the x86_64-vibeos-musl cross compiler against the VibeOS
 * sysroot (ROADMAP §"Toolchain integration"), not host musl flags. Proves the
 * toolchain produces a working VibeOS binary: links musl's libc + crt from the
 * sysroot and runs over VibeOS's Linux-ABI syscalls.
 */
#include <stdio.h>
#include <vibeos.h>

int main(int argc, char **argv) {
#ifdef __vibeos__
    printf("vibehello: compiled by x86_64-vibeos-musl-gcc for VibeOS\n");
    printf("  target loader: %s\n", VIBEOS_DYNAMIC_LINKER);
#else
    printf("vibehello: NOT built for the vibeos target?!\n");
#endif
    printf("  argc=%d argv[0]=%s\n", argc, argv[0]);
    return 0;
}
