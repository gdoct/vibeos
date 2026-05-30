#include "kernel.h"
#include "csprng.h"
#include "spinlock.h"
#include "timer.h"

/*
 * ChaCha20 DRBG. See csprng.h.
 *
 * Construction (à la OpenBSD arc4random): keep a 256-bit key + 96-bit nonce +
 * 32-bit block counter. To produce output, run ChaCha20 keystream blocks; after
 * each request, derive a fresh key+nonce from one more keystream block and zero
 * the counter. Past output therefore can't be regenerated from the live state.
 *
 * Seed/reseed entropy: RDRAND (when present) + RDTSC timing jitter, folded into
 * the key. csprng_add_entropy() lets a hardware RNG (virtio-rng) contribute.
 */

/* ---- ChaCha20 core ---- */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d)                                  \
    do {                                                \
        a += b; d ^= a; d = ROTL32(d, 16);              \
        c += d; b ^= c; b = ROTL32(b, 12);              \
        a += b; d ^= a; d = ROTL32(d, 8);               \
        c += d; b ^= c; b = ROTL32(b, 7);               \
    } while (0)

static void chacha20_block(const uint32_t key[8], uint32_t counter,
                           const uint32_t nonce[3], uint8_t out[64]) {
    static const uint32_t C[4] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };
    uint32_t s[16], x[16];
    s[0] = C[0]; s[1] = C[1]; s[2] = C[2]; s[3] = C[3];
    for (int i = 0; i < 8; i++) s[4 + i] = key[i];
    s[12] = counter;
    s[13] = nonce[0]; s[14] = nonce[1]; s[15] = nonce[2];
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {           /* 20 rounds = 10 double-rounds */
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[i * 4 + 0] = (uint8_t)(v);
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

/* ---- entropy sources ---- */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));
    return (ecx >> 30) & 1u;
}

static int rdrand64(uint64_t *out) {
    for (int i = 0; i < 10; i++) {
        uint64_t v; uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

/* Harvest a 64-bit jitter sample from the variance between RDTSC and a short
   busy loop: on real silicon (and QEMU+TCG/KVM) successive deltas wobble. We
   fold the low bits of many deltas, where the unpredictability lives. */
static uint64_t timing_jitter(void) {
    uint64_t acc = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t a = rdtsc();
        /* a little variable work so the next read isn't a fixed stride */
        for (volatile int k = 0; k < (int)((a & 7) + 1); k++) { }
        uint64_t b = rdtsc();
        acc = (acc << 1) | ((b - a) & 1);     /* keep only the noisy LSB */
        acc ^= ROTL32((uint32_t)(b ^ a), i & 31);
    }
    return acc ^ (acc << 32);
}

/* ---- DRBG state ---- */

static uint32_t  g_key[8];
static uint32_t  g_nonce[3];
static uint32_t  g_ctr;
static int       g_seeded;
static uint8_t   g_isn_secret[32];             /* keys the RFC 6528 ISN hash */
static spinlock_t g_lock = SPINLOCK_INIT;
static int       g_have_rdrand;

/* Fold 32 bytes of fresh entropy into the key (caller holds g_lock). */
static void mix_entropy(const uint8_t seed[32]) {
    for (int i = 0; i < 8; i++) {
        uint32_t s = (uint32_t)seed[i * 4 + 0]
                   | ((uint32_t)seed[i * 4 + 1] << 8)
                   | ((uint32_t)seed[i * 4 + 2] << 16)
                   | ((uint32_t)seed[i * 4 + 3] << 24);
        g_key[i] ^= s;
    }
    /* Run a block and re-key from it so the mix diffuses across the whole key. */
    uint8_t b[64];
    chacha20_block(g_key, g_ctr++, g_nonce, b);
    for (int i = 0; i < 8; i++)
        g_key[i] = (uint32_t)b[i*4] | ((uint32_t)b[i*4+1] << 8)
                 | ((uint32_t)b[i*4+2] << 16) | ((uint32_t)b[i*4+3] << 24);
    g_nonce[0] = (uint32_t)b[32] | ((uint32_t)b[33] << 8) | ((uint32_t)b[34] << 16) | ((uint32_t)b[35] << 24);
    g_nonce[1] = (uint32_t)b[36] | ((uint32_t)b[37] << 8) | ((uint32_t)b[38] << 16) | ((uint32_t)b[39] << 24);
    g_nonce[2] = (uint32_t)b[40] | ((uint32_t)b[41] << 8) | ((uint32_t)b[42] << 16) | ((uint32_t)b[43] << 24);
    g_ctr = 0;
}

/* Gather a 32-byte seed from all available sources (caller holds g_lock). */
static void gather_seed(uint8_t seed[32]) {
    uint64_t acc[4];
    for (int i = 0; i < 4; i++) acc[i] = 0x9E3779B97F4A7C15ULL * (i + 1);
    for (int i = 0; i < 4; i++) {
        uint64_t r = 0;
        if (g_have_rdrand) rdrand64(&r);
        acc[i] ^= r;
        acc[i] ^= timing_jitter();
        acc[i] ^= rdtsc() << (i + 1);
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            seed[i * 8 + j] = (uint8_t)(acc[i] >> (j * 8));
}

/* Emit one block of keystream and advance (caller holds g_lock). */
static void gen_block(uint8_t out[64]) {
    chacha20_block(g_key, g_ctr++, g_nonce, out);
}

static void rekey_locked(void) {
    uint8_t b[64];
    gen_block(b);
    for (int i = 0; i < 8; i++)
        g_key[i] = (uint32_t)b[i*4] | ((uint32_t)b[i*4+1] << 8)
                 | ((uint32_t)b[i*4+2] << 16) | ((uint32_t)b[i*4+3] << 24);
    g_nonce[0] = (uint32_t)b[32] | ((uint32_t)b[33] << 8) | ((uint32_t)b[34] << 16) | ((uint32_t)b[35] << 24);
    g_nonce[1] = (uint32_t)b[36] | ((uint32_t)b[37] << 8) | ((uint32_t)b[38] << 16) | ((uint32_t)b[39] << 24);
    g_nonce[2] = (uint32_t)b[40] | ((uint32_t)b[41] << 8) | ((uint32_t)b[42] << 16) | ((uint32_t)b[43] << 24);
    g_ctr = 0;
}

void csprng_init(void) {
    spin_lock(&g_lock);
    g_have_rdrand = cpu_has_rdrand();
    for (int i = 0; i < 8; i++) g_key[i] = 0;
    g_nonce[0] = g_nonce[1] = g_nonce[2] = 0;
    g_ctr = 0;
    uint8_t seed[32];
    gather_seed(seed);
    mix_entropy(seed);
    /* Derive the long-lived ISN secret from the freshly-seeded stream. */
    uint8_t b[64];
    gen_block(b);
    for (int i = 0; i < 32; i++) g_isn_secret[i] = b[i];
    rekey_locked();
    g_seeded = 1;
    spin_unlock(&g_lock);
    kprintf("[csprng] ChaCha20 DRBG seeded (rdrand=%s, jitter=yes)\n",
            g_have_rdrand ? "yes" : "no");
}

void csprng_add_entropy(const void *buf, size_t n) {
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = 0;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) seed[i % 32] ^= p[i];
    spin_lock(&g_lock);
    if (!g_seeded) { spin_unlock(&g_lock); csprng_init(); spin_lock(&g_lock); }
    mix_entropy(seed);
    spin_unlock(&g_lock);
}

void csprng_bytes(void *buf, size_t n) {
    if (!g_seeded) csprng_init();
    uint8_t *out = (uint8_t *)buf;
    spin_lock(&g_lock);
    while (n > 0) {
        uint8_t b[64];
        gen_block(b);
        size_t c = n < 64 ? n : 64;
        for (size_t i = 0; i < c; i++) out[i] = b[i];
        out += c; n -= c;
    }
    rekey_locked();                 /* forward secrecy */
    spin_unlock(&g_lock);
}

uint32_t csprng_u32(void) { uint32_t v; csprng_bytes(&v, sizeof v); return v; }
uint64_t csprng_u64(void) { uint64_t v; csprng_bytes(&v, sizeof v); return v; }

uint32_t csprng_tcp_isn(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport) {
    if (!g_seeded) csprng_init();
    uint32_t key[8], nonce[3], counter;
    spin_lock(&g_lock);
    for (int i = 0; i < 8; i++)
        key[i] = (uint32_t)g_isn_secret[i*4] | ((uint32_t)g_isn_secret[i*4+1] << 8)
               | ((uint32_t)g_isn_secret[i*4+2] << 16) | ((uint32_t)g_isn_secret[i*4+3] << 24);
    spin_unlock(&g_lock);
    nonce[0] = lip;
    nonce[1] = rip;
    nonce[2] = ((uint32_t)lport << 16) | rport;
    counter  = lip ^ rip ^ ((uint32_t)lport << 16) ^ rport;
    uint8_t b[64];
    chacha20_block(key, counter, nonce, b);
    uint32_t f = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
               | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    /* Monotonic term: ~65536 per 10 ms tick (RFC 6528 uses a 4 µs clock). */
    uint32_t tick = (uint32_t)timer_ticks();
    return f + (tick << 16);
}
