/* §4 dynamic-linking test: an unmodified dynamically-linked musl PIE.
 * Loaded via PT_INTERP=/lib/ld-musl-x86_64.so.1; the kernel maps the program
 * and the dynamic linker, which self-relocates and runs us. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("dynamic musl says hello! argc=%d argv0=%s\n", argc, argv[0]);
    char *p = malloc(64);
    strcpy(p, "heap+libc work under the dynamic linker");
    printf("  %s\n", p);
    free(p);
    return 0;
}
