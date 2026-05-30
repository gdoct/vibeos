#include "kernel.h"
#include "random.h"

/*
 * Tiny non-cryptographic RNG (ROADMAP §5 plumbing). See random.h.
 *
 * Two sources: the CPU's RDRAND instruction when CPUID advertises it, with a
 * seeded xorshift64* PRNG as the fallback (and to whiten/mix in either case).
 * The PRNG state is seeded from TSC + a couple of RDRAND/RDTSC draws so it is
 * not identical across boots.
 */

static uint64_t g_state = 0x9E3779B97F4A7C15ULL;  /* nonzero default */
static int      g_have_rdrand = 0;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* CPUID leaf 1, ECX bit 30 = RDRAND. */
static int cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));
    return (ecx >> 30) & 1u;
}

/* One RDRAND draw with the architectural retry loop; 0 on persistent failure. */
static int rdrand64(uint64_t *out) {
    for (int i = 0; i < 10; i++) {
        uint64_t v; uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

/* xorshift64* — fast, decent distribution, never returns the all-zero cycle. */
static uint64_t xorshift64star(void) {
    uint64_t x = g_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

void krandom_init(void) {
    g_have_rdrand = cpu_has_rdrand();
    uint64_t seed = rdtsc();
    uint64_t r;
    if (g_have_rdrand && rdrand64(&r)) seed ^= r;
    seed ^= rdtsc() << 1;
    if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;
    g_state = seed;
    /* Stir so early draws don't echo the raw seed. */
    for (int i = 0; i < 16; i++) (void)xorshift64star();
    kprintf("[random] seeded (rdrand=%s)\n", g_have_rdrand ? "yes" : "no");
}

uint64_t krandom_u64(void) {
    uint64_t prng = xorshift64star();
    uint64_t hw;
    if (g_have_rdrand && rdrand64(&hw)) return hw ^ prng;  /* mix both sources */
    return prng;
}

void krandom_bytes(void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        uint64_t v = krandom_u64();
        size_t chunk = n < 8 ? n : 8;
        for (size_t i = 0; i < chunk; i++) p[i] = (uint8_t)(v >> (i * 8));
        p += chunk; n -= chunk;
    }
}
