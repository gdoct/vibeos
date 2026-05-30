#ifndef VIBEOS_RANDOM_H
#define VIBEOS_RANDOM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tiny kernel randomness (ROADMAP §5 plumbing).
 *
 * NOT cryptographic. Prefers the CPU's RDRAND when present (QEMU exposes it),
 * and otherwise falls back to a seeded xorshift64* PRNG. Good enough for the
 * things plain networking needs variety for — TCP ISNs, ephemeral ports, DHCP
 * xids — and for seeding a libc's stack canary (auxv AT_RANDOM). A real entropy
 * pool + CSPRNG is a later, TLS-gated concern.
 */

void     krandom_init(void);            /* seed the fallback PRNG (call once, early) */
uint64_t krandom_u64(void);             /* 64 random bits */
void     krandom_bytes(void *buf, size_t n);   /* fill buf with random bytes */

#ifdef __cplusplus
}
#endif

#endif
