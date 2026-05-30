#ifndef VIBEOS_CSPRNG_H
#define VIBEOS_CSPRNG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cryptographic RNG (ROADMAP §2): a ChaCha20-based DRBG seeded from real
 * entropy — RDRAND, RDTSC timing jitter, and any bytes a virtio-rng driver
 * feeds in via csprng_add_entropy(). Each request emits ChaCha20 keystream and
 * then rekeys from a fresh block, so emitted output can't reconstruct the state
 * (forward secrecy). SMP-safe (own spinlock).
 *
 * This replaces krandom() for security-sensitive values — TCP ISNs today, TLS
 * later. krandom() (random.c) stays for non-crypto variety / AT_RANDOM seeding.
 */

void     csprng_init(void);                    /* gather initial seed (call once, early) */
void     csprng_bytes(void *buf, size_t n);    /* fill buf with CSPRNG output */
uint32_t csprng_u32(void);
uint64_t csprng_u64(void);

/* Mix external entropy into the pool and reseed (e.g. from virtio-rng). */
void     csprng_add_entropy(const void *buf, size_t n);

/* RFC 6528 TCP initial sequence number: a per-4-tuple keyed hash plus a
   monotonic timer term, so ISNs are unpredictable off-path yet don't collide
   with a recent incarnation of the same connection. Host byte order. */
uint32_t csprng_tcp_isn(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport);

#ifdef __cplusplus
}
#endif

#endif
