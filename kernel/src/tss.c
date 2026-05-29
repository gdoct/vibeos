#include "kernel.h"
#include "idt.h"
#include "usermode.h"

/*
 * Task State Segment (ROADMAP §3, Phase 1).
 *
 * In long mode the TSS no longer holds a task context — the CPU only reads
 * rsp0..rsp2 (the stack pointers to load on an inward privilege change) and
 * the IST entries. We use rsp0: when an interrupt or exception fires while
 * the CPU is in ring 3, it switches to TSS.rsp0 before pushing the trap
 * frame, so we land on the current task's kernel stack rather than the user
 * stack. (Syscalls switch stacks themselves — see syscall.S — because the
 * SYSCALL instruction does not consult the TSS.)
 */

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

static tss_t g_tss;

/* The two GDT slots reserved for the TSS descriptor (gdt.S). */
extern "C" uint64_t gdt_tss[2];

void tss_set_rsp0(uint64_t rsp0) { g_tss.rsp0 = rsp0; }

void tss_init(void) {
    kmemset(&g_tss, 0, sizeof(g_tss));
    /* No I/O permission bitmap: point it past the segment limit. */
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);

    uint64_t base  = (uint64_t)(uintptr_t)&g_tss;
    uint32_t limit = sizeof(g_tss) - 1;

    /* 64-bit TSS descriptor (16 bytes across gdt_tss[0..1]).
       low quad:  limit[15:0] | base[15:0]<<16 | base[23:16]<<32 |
                  type 0x9 (available 64-bit TSS) + P, at byte 5 (<<40) |
                  limit[19:16]<<48 | base[31:24]<<56
       high quad: base[63:32]. */
    uint64_t lo = 0;
    lo |= (uint64_t)(limit & 0xFFFF);
    lo |= (uint64_t)(base & 0xFFFFFF) << 16;
    lo |= (uint64_t)0x89 << 40;                 /* P=1, DPL=0, type=9 */
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;
    lo |= (uint64_t)((base >> 24) & 0xFF) << 56;
    gdt_tss[0] = lo;
    gdt_tss[1] = (base >> 32) & 0xFFFFFFFF;

    __asm__ volatile("ltr %w0" :: "r"((uint16_t)TSS_SEL));
    kprintf("[tss] installed, rsp0 latch ready (sel=%x)\n", TSS_SEL);
}
