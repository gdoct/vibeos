#include "kernel.h"
#include "pmm.h"
#include "device.h"
#include "fb.h"
#include "block.h"
#include "idt.h"
#include "irq.h"
#include "timer.h"
#include "task.h"
#include "kmalloc.h"
#include "paging.h"
#include "apic.h"
#include "fs.h"
#include "usermode.h"
#include "../../boot/include/bootinfo.h"

extern "C" block_device_t *ramdisk_init(uint64_t num_blocks);
extern "C" block_device_t *virtio_blk_init(void);
extern "C" uint64_t        virtio_blk_completions(void);

static volatile int g_alive_workers = 0;
static block_device_t *g_demo_vblk = nullptr;

static void demo_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 3; i++) {
        kprintf("[w%d] iter %d at tick %lu\n",
                id, i, (unsigned long)timer_ticks());
        /* Sleep ~500 ms by blocking on the timer sleeper list. The task
           is off the run queue the whole time; with no other ready task
           the idle loop hlts, so the CPU is genuinely idle between the
           workers' wakeups rather than spinning. */
        ksleep_ms(500);
    }
    g_alive_workers--;
}

/* Issues a series of blocking block_reads on virtio-blk. Each read now
   sleeps the task until the device's completion IRQ fires, so the CPU
   spends the disk latency running the other workers (or idling) instead
   of spinning on used_idx. */
static void io_worker(void *arg) {
    (void)arg;
    uint8_t buf[512];
    uint64_t before = virtio_blk_completions();
    for (uint64_t lba = 0; lba < 8; lba++) {
        int r = block_read(g_demo_vblk, lba, 1, buf);
        kprintf("[io] read lba %lu -> %d (tick %lu, completions=%lu)\n",
                (unsigned long)lba, r, (unsigned long)timer_ticks(),
                (unsigned long)virtio_blk_completions());
    }
    kprintf("[io] done: %lu IRQ-driven completions\n",
            (unsigned long)(virtio_blk_completions() - before));
    g_alive_workers--;
}

/* The userspace init program, embedded as a blob (user_blob.o / user/init.c).
   ROADMAP §3 Milestone A loads it from memory; Milestone B reads it from the
   filesystem via the host populate tool. */
extern "C" const uint8_t init_elf_start[];
extern "C" const uint8_t init_elf_end[];
extern "C" const uint8_t hello_elf_start[];
extern "C" const uint8_t hello_elf_end[];

/* Embedded program table — the stand-in for /bin until the FS holds it
   (ROADMAP §3 B.3). execve resolves names through prog_lookup. */
struct embedded_prog { const char *path; const uint8_t *start; const uint8_t *end; };
static const embedded_prog g_progs[] = {
    { "/bin/init",  init_elf_start,  init_elf_end  },
    { "/bin/hello", hello_elf_start, hello_elf_end },
};

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const void *prog_lookup(const char *path, uint64_t *size) {
    for (unsigned i = 0; i < sizeof(g_progs) / sizeof(g_progs[0]); i++)
        if (streq(path, g_progs[i].path)) {
            *size = (uint64_t)(g_progs[i].end - g_progs[i].start);
            return g_progs[i].start;
        }
    return nullptr;
}

/* Load /init into the (now free) low half and drop to ring 3. Runs as a task,
   so it owns a kernel stack for syscalls/IRQs; on exit() the task dies and the
   scheduler moves on. */
static void init_launch(void *arg) {
    (void)arg;
    /* init gets its own address space (low half private, kernel half shared). */
    vmspace_t *vm = vmspace_create();
    if (!vm) panic("init: vmspace_create failed");
    task_set_vmspace(vm);             /* attach + make active for the load below */

    uint64_t size = (uint64_t)(init_elf_end - init_elf_start);
    uint64_t entry, rsp;
    int r = user_load(vm, init_elf_start, size, "/bin/init", &entry, &rsp);
    if (r != 0) panic("init: user_load failed (%d)", r);
    kprintf("[init] loaded init (%lu bytes), entry=%lx rsp=%lx -> ring 3\n",
            (unsigned long)size, (unsigned long)entry, (unsigned long)rsp);
    enter_user(entry, rsp);
}

void scheduler_demo(void) {
    sched_init();
    g_alive_workers = 2;
    task_create("w0", demo_worker, (void *)(intptr_t)0);
    task_create("w1", demo_worker, (void *)(intptr_t)1);
    if (g_demo_vblk) {
        g_alive_workers++;
        task_create("io", io_worker, nullptr);
    }
    task_create("init", init_launch, nullptr);   /* userspace init in ring 3 */
    /* Boot task blocks itself between checks too, instead of spinning. */
    while (g_alive_workers > 0) ksleep_ms(20);
    kprintf("[demo] all workers exited at tick %lu\n",
            (unsigned long)timer_ticks());
}

static void check_bootinfo(const BootInfo *bi) {
    if (bi->magic != BOOTINFO_MAGIC)
        panic("bad bootinfo magic %lx", (unsigned long)bi->magic);
    if (bi->version != BOOTINFO_VERSION)
        panic("bootinfo version %u, expected %u", bi->version, BOOTINFO_VERSION);
    if (bi->size < sizeof(BootInfo))
        panic("bootinfo size %u < %lu", bi->size, (unsigned long)sizeof(BootInfo));
}

static void paint_splash(fb_device_t *fb) {
    /* Background gradient — dark teal top, near-black bottom. */
    for (uint32_t y = 0; y < fb->height; y++) {
        uint8_t t = (uint8_t)((uint64_t)y * 30 / (fb->height ? fb->height : 1));
        uint32_t color = fb_rgb(fb, 0, (uint8_t)(40 - t), (uint8_t)(60 - t));
        fb->fill_rect(fb, 0, y, fb->width, 1, color);
    }

    uint32_t white = fb_rgb(fb, 0xE0, 0xE0, 0xE0);
    uint32_t teal  = fb_rgb(fb, 0x10, 0x40, 0x50);

    /* Title bar. */
    fb->fill_rect(fb, 0, 0, fb->width, FB_FONT_H * 2, teal);
    fb->draw_text(fb, FB_FONT_W, FB_FONT_H/2,
                  "MyOS kernel", white, teal);
}

static void selftest_block(block_device_t *bd) {
    uint8_t wbuf[512];
    uint8_t rbuf[512];

    for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i * 7 + 3);

    int r = block_write(bd, 7, 1, wbuf);
    if (r != BLK_OK) panic("block write failed: %d", r);

    kmemset(rbuf, 0, sizeof(rbuf));
    r = block_read(bd, 7, 1, rbuf);
    if (r != BLK_OK) panic("block read failed: %d", r);

    if (kmemcmp(wbuf, rbuf, 512) != 0)
        panic("block read/write mismatch");

    kprintf("[selftest] block %s: 1x%u-byte sector round-trip OK\n",
            bd->dev.name, bd->block_size);
}

/* Deterministic byte pattern keyed by absolute file offset, for large-file
   round-trip checks. */
static uint8_t fs_pat(uint64_t off) { return (uint8_t)(off * 2654435761u + 0xABu); }

/* End-to-end filesystem exercise: format, create a dir + files (sized to reach
   single-, double-, and triple-indirect blocks, including a >4 GiB sparse
   file), read them back, list the dir, unlink, then prove persistence across a
   clean remount and recovery across a forced-dirty fsck. Runs on the boot stack
   with virtio polling (no scheduler yet), like selftest_block above. */
static void selftest_fs(block_device_t *dev) {
    const char *msg = "Hello, MyFS!\n";
    uint32_t mlen = (uint32_t)kstrlen(msg);
    uint32_t spb  = FS_BLOCK_SIZE / dev->block_size;
    int r, fd, got;

    r = fs_mount(dev);
    if (r != FS_OK) panic("fs: mount failed (%d)", r);

    /* On a reboot the volume already holds /dir from a previous run — that is
       itself proof of cross-reboot persistence, so EEXIST is fine here. */
    r = fs_mkdir("/dir");
    if (r != FS_OK && r != FS_EEXIST) panic("fs: mkdir /dir failed (%d)", r);

    /* small file round-trip (TRUNC so a re-run starts clean) */
    fd = fs_open("/dir/hello.txt", FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) panic("fs: open hello.txt failed (%d)", fd);
    if (fs_write(fd, msg, mlen) != (int)mlen) panic("fs: short write");
    fs_close(fd);

    char small[32];
    fd = fs_open("/dir/hello.txt", 0);
    if (fd < 0) panic("fs: reopen hello.txt failed (%d)", fd);
    got = fs_read(fd, small, sizeof small);
    fs_close(fd);
    if (got != (int)mlen || kmemcmp(small, msg, mlen) != 0)
        panic("fs: hello.txt round-trip mismatch (got %d)", got);

    /* large file: 64 KiB = 16 blocks > 13 direct, so single-indirect kicks in */
    const uint32_t BIG = 64 * 1024;
    uint8_t *wbuf = (uint8_t *)kmalloc(BIG);
    uint8_t *rbuf = (uint8_t *)kmalloc(BIG);
    if (!wbuf || !rbuf) panic("fs: selftest out of memory");
    for (uint32_t i = 0; i < BIG; i++) wbuf[i] = (uint8_t)(i * 31u + 7u);

    fd = fs_open("/dir/big.bin", FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) panic("fs: open big.bin failed (%d)", fd);
    if (fs_write(fd, wbuf, BIG) != (int)BIG) panic("fs: big.bin short write");
    fs_close(fd);

    fd = fs_open("/dir/big.bin", 0);
    kmemset(rbuf, 0, BIG);
    got = fs_read(fd, rbuf, BIG);
    fs_close(fd);
    if (got != (int)BIG || kmemcmp(wbuf, rbuf, BIG) != 0)
        panic("fs: big.bin round-trip mismatch (got %d)", got);
    kprintf("[selftest] fs: 64 KiB indirect-block file round-trip OK\n");

    /* 5 MiB file: 1280 blocks > 1037 (13 direct + 1024 single-indirect), so
       this crosses into double-indirect. Written/verified in 64 KiB chunks to
       avoid a multi-MiB compare buffer. */
    const uint64_t HUGE = 5ull * 1024 * 1024;
    fd = fs_open("/dir/huge.bin", FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) panic("fs: open huge.bin failed (%d)", fd);
    for (uint64_t pos = 0; pos < HUGE; pos += BIG) {
        uint32_t chunk = (uint32_t)(HUGE - pos < BIG ? HUGE - pos : BIG);
        for (uint32_t i = 0; i < chunk; i++) wbuf[i] = fs_pat(pos + i);
        if (fs_write(fd, wbuf, chunk) != (int)chunk)
            panic("fs: huge.bin short write @%lu", (unsigned long)pos);
    }
    fs_close(fd);
    fd = fs_open("/dir/huge.bin", 0);
    for (uint64_t pos = 0; pos < HUGE; pos += BIG) {
        uint32_t chunk = (uint32_t)(HUGE - pos < BIG ? HUGE - pos : BIG);
        kmemset(rbuf, 0, chunk);
        if (fs_read(fd, rbuf, chunk) != (int)chunk)
            panic("fs: huge.bin short read @%lu", (unsigned long)pos);
        for (uint32_t i = 0; i < chunk; i++)
            if (rbuf[i] != fs_pat(pos + i))
                panic("fs: huge.bin mismatch @%lu", (unsigned long)(pos + i));
    }
    fs_close(fd);
    kprintf("[selftest] fs: 5 MiB double-indirect file round-trip OK\n");

    /* Past 4 GiB: seek to a 5 GiB offset (file block ~1.31M, in triple-indirect
       territory) and write a marker. Proves 64-bit offsets/size + the
       triple-indirect path. The file is sparse, so only a handful of blocks are
       physically allocated on the 8 GiB volume. */
    const uint64_t FAR = 5ull * 1024 * 1024 * 1024;     /* 5 GiB */
    const char *mark   = "past-4GiB!";
    uint32_t klen      = (uint32_t)kstrlen(mark);
    fd = fs_open("/dir/sparse.bin", FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) panic("fs: open sparse.bin failed (%d)", fd);
    if (fs_seek(fd, FAR) != FS_OK) panic("fs: seek to 5 GiB failed");
    if (fs_write(fd, mark, klen) != (int)klen) panic("fs: sparse.bin write failed");
    fs_close(fd);

    fd = fs_open("/dir/sparse.bin", 0);
    kmemset(rbuf, 0xFF, 16);
    if (fs_read(fd, rbuf, 16) != 16) panic("fs: sparse hole short read");
    for (int i = 0; i < 16; i++) if (rbuf[i] != 0) panic("fs: sparse hole not zero");
    if (fs_seek(fd, FAR) != FS_OK) panic("fs: re-seek to 5 GiB failed");
    kmemset(rbuf, 0, 32);
    got = fs_read(fd, rbuf, klen);
    fs_close(fd);
    if (got != (int)klen || kmemcmp(rbuf, mark, klen) != 0)
        panic("fs: 5 GiB marker round-trip mismatch (got %d)", got);
    kprintf("[selftest] fs: 5 GiB-offset triple-indirect write/read OK (file > 4 GiB)\n");

    /* directory listing */
    fs_dirent_t ents[8];
    int ne = fs_readdir("/dir", ents, 8);
    kprintf("[selftest] fs: /dir lists %d entries:", ne);
    for (int i = 0; i < ne; i++)
        kprintf(" %s%s", ents[i].name, ents[i].type == FT_DIR ? "/" : "");
    kprintf("\n");

    /* unlink round-trip */
    r = fs_create("/dir/tmp");
    if (r != FS_OK && r != FS_EEXIST)    panic("fs: create tmp failed (%d)", r);
    if (fs_unlink("/dir/tmp") != FS_OK)  panic("fs: unlink tmp failed");
    if (fs_open("/dir/tmp", 0) != FS_ENOENT) panic("fs: tmp survived unlink");

    /* clean unmount + remount: data persists, no fsck */
    if (fs_unmount() != FS_OK) panic("fs: unmount failed");
    if (fs_mount(dev) != FS_OK) panic("fs: remount failed");
    fd = fs_open("/dir/hello.txt", 0);
    if (fd < 0) panic("fs: hello.txt gone after clean remount (%d)", fd);
    kmemset(small, 0, sizeof small);
    got = fs_read(fd, small, sizeof small);
    fs_close(fd);
    if (got != (int)mlen || kmemcmp(small, msg, mlen) != 0)
        panic("fs: persistence check failed");
    kprintf("[selftest] fs: clean remount persisted /dir/hello.txt\n");

    /* forced-dirty fsck: set the dirty flag and scribble the data bitmap to
       all-free on disk, then remount. fsck must rebuild it from the inodes
       (the source of truth) and leave the data intact. */
    block_read(dev, 0, spb, rbuf);
    uint32_t db_blk = ((superblock_t *)rbuf)->data_bitmap_blk;
    ((superblock_t *)rbuf)->dirty = 1;
    block_write(dev, 0, spb, rbuf);              /* mark unclean */
    kmemset(rbuf, 0, FS_BLOCK_SIZE);
    block_write(dev, (uint64_t)db_blk * spb, spb, rbuf);   /* wipe first data-bitmap block */

    if (fs_mount(dev) != FS_OK) panic("fs: dirty remount failed");
    fd = fs_open("/dir/big.bin", 0);
    if (fd < 0) panic("fs: big.bin gone after fsck (%d)", fd);
    kmemset(rbuf, 0, BIG);
    got = fs_read(fd, rbuf, BIG);
    fs_close(fd);
    /* Recompute big.bin's pattern: wbuf was reused by the 5 MiB test above. */
    if (got != (int)BIG) panic("fs: big.bin short read after fsck (got %d)", got);
    for (uint32_t i = 0; i < BIG; i++)
        if (rbuf[i] != (uint8_t)(i * 31u + 7u))
            panic("fs: big.bin corrupted by fsck @%u", i);

    /* The double- and triple-indirect files must survive the bitmap rebuild too
       (fsck walks all three indirect levels to re-mark their blocks). */
    fd = fs_open("/dir/huge.bin", 0);
    if (fd < 0) panic("fs: huge.bin gone after fsck (%d)", fd);
    for (uint64_t pos = 0; pos < HUGE; pos += BIG) {
        uint32_t chunk = (uint32_t)(HUGE - pos < BIG ? HUGE - pos : BIG);
        kmemset(rbuf, 0, chunk);
        if (fs_read(fd, rbuf, chunk) != (int)chunk) panic("fs: huge.bin short read post-fsck");
        for (uint32_t i = 0; i < chunk; i++)
            if (rbuf[i] != fs_pat(pos + i)) panic("fs: huge.bin corrupted by fsck @%lu",
                                                  (unsigned long)(pos + i));
    }
    fs_close(fd);
    fd = fs_open("/dir/sparse.bin", 0);
    if (fd < 0) panic("fs: sparse.bin gone after fsck (%d)", fd);
    if (fs_seek(fd, FAR) != FS_OK) panic("fs: post-fsck seek to 5 GiB failed");
    kmemset(rbuf, 0, 32);
    got = fs_read(fd, rbuf, klen);
    fs_close(fd);
    if (got != (int)klen || kmemcmp(rbuf, mark, klen) != 0)
        panic("fs: sparse.bin corrupted by fsck (got %d)", got);

    /* allocation still works (rebuilt bitmap has no phantom free blocks) */
    fd = fs_open("/dir/after_fsck.txt", FS_O_CREATE);
    if (fd < 0 || fs_write(fd, msg, mlen) != (int)mlen)
        panic("fs: write after fsck failed");
    fs_close(fd);
    kprintf("[selftest] fs: fsck rebuilt the wiped bitmap, multi-level data intact\n");

    fs_unmount();
    kfree(wbuf);
    kfree(rbuf);
}

static void selftest_paging(void) {
    /* Direct map: the same physical bytes must be visible through the
       kernel's real (high-half) address and through PHYS_OFFSET + phys.
       There is no identity map anymore — the low half belongs to userspace. */
    uint64_t self_pa = kva_to_phys((const void *)&selftest_paging);
    volatile uint32_t *high   = (volatile uint32_t *)&selftest_paging;
    volatile uint32_t *direct = (volatile uint32_t *)phys_to_virt(self_pa);
    if (*high != *direct)
        panic("paging: direct map mismatch %x != %x", *high, *direct);

    /* The low half must be unmapped — it belongs to userspace now. Querying
       (not dereferencing) keeps this safe. */
    uint64_t lo;
    if (paging_query(0x100000, &lo))
        panic("paging: low half still mapped (%lx) — identity not dropped", lo);

    /* vmap round-trip: map a scratch page at a scratch VA, write through
       it, observe the write through the identity alias, then unmap. */
    uint64_t pa = pmm_alloc_page();
    uint64_t va = KSTACK_REGION - 0x40000000ULL;   /* away from stacks */
    vmap(va, pa, 1, PTE_P | PTE_W);
    uint64_t got = 0;
    if (!paging_query(va, &got) || got != pa)
        panic("paging: query after vmap gave %lx, want %lx",
              (unsigned long)got, (unsigned long)pa);
    *(volatile uint32_t *)va = 0xC0FFEE42;
    if (*(volatile uint32_t *)phys_to_virt(pa) != 0xC0FFEE42)
        panic("paging: write via vmap not visible at phys");
    vunmap(va, 1);
    if (paging_query(va, &got))
        panic("paging: still mapped after vunmap");
    pmm_free_page(pa);

    /* Guard page: a fresh kernel stack must be mapped, with the page just
       below its base unmapped. */
    uint64_t base, dummy;
    uint64_t top = kstack_alloc(4, &base);
    (void)top;
    if (!paging_query(base, &dummy))
        panic("paging: stack base not mapped");
    if (paging_query(base - PAGE_SIZE, &dummy))
        panic("paging: stack guard page is mapped (no protection!)");
    vunmap(base, 4);   /* leak the phys pages + VA range; fine for a test */

    kprintf("[selftest] paging: direct map + vmap round-trip + stack guard OK\n");
}

static void selftest_heap(void) {
    /* Page freelist: a freed single page must be the next one handed out. */
    uint64_t a = pmm_alloc_page();
    pmm_free_page(a);
    uint64_t b = pmm_alloc_page();
    if (a != b) panic("pmm freelist: freed %lx, got %lx", a, b);
    pmm_free_page(b);

    /* Slab: small allocations are distinct and writable, and a freed
       block of a class is reused by the next same-class request. */
    char *p = (char *)kmalloc(100);
    char *q = (char *)kmalloc(100);
    if (!p || !q || p == q) panic("kmalloc: bad small alloc %p %p", p, q);
    for (int i = 0; i < 100; i++) p[i] = (char)i;   /* fault check */
    kfree(p);
    char *r = (char *)kmalloc(100);
    if (r != p) panic("kmalloc: class not reused (%p vs %p)", r, p);
    kfree(q);
    kfree(r);

    /* Large path: bigger than the top class, served by whole pages. */
    void *big = kmalloc(9000);
    if (!big) panic("kmalloc: large alloc failed");
    kmemset(big, 0xAB, 9000);
    kfree(big);

    if (kmalloc_in_use() != 0)
        panic("kmalloc: %lu bytes leaked", (unsigned long)kmalloc_in_use());

    kprintf("[selftest] heap: freelist reuse + slab + large + free OK\n");
}

extern "C" void kmain(BootInfo *bi) {
    /* The bootloader hands us a low physical BootInfo*. Reach it through the
       direct map so it stays valid after paging_init drops the low identity
       (the bootstrap tables in start.S already provide the direct map). */
    bi = (BootInfo *)phys_to_virt((uint64_t)(uintptr_t)bi);

    serial_init();
    kprintf("\n[kernel] MyOS booting (BootInfo @ %p)\n", bi);

    gdt_init();
    idt_init();
    tss_init();        /* ring-3 -> ring-0 stack switch (TSS.rsp0) */
    syscall_init();    /* SYSCALL/SYSRET entry */

    check_bootinfo(bi);
    kprintf("[kernel] kernel image: %lx..%lx (%lu KiB)\n",
            (unsigned long)bi->kernel_phys_base,
            (unsigned long)bi->kernel_phys_end,
            (unsigned long)((bi->kernel_phys_end - bi->kernel_phys_base) >> 10));
    kprintf("[kernel] rsdp=%lx runtime_services=%lx\n",
            (unsigned long)bi->rsdp, (unsigned long)bi->runtime_services);
    kprintf("[kernel] fb %ux%u pitch=%u fmt=%u base=%lx\n",
            bi->fb.width, bi->fb.height, bi->fb.pitch, bi->fb.format,
            (unsigned long)bi->fb.base);

    pmm_init(bi);
    paging_init(bi);
    selftest_paging();
    selftest_heap();

    fb_device_t *fb = fb_init(&bi->fb);
    paint_splash(fb);

    /* Interrupt controller bring-up. irq_init remaps + masks the 8259 so
       it's in a known-quiet state; apic_init then (if the MADT is usable)
       takes over as the active controller, leaving the 8259 fully masked.
       Done before device init so a driver can wire its IRQ here and have
       it routed. Interrupts stay globally masked (IF=0) until irq_enable
       below. */
    irq_init();
    timer_init(100);
    if (!apic_init(bi, 100)) {
        timer_start_pit();          /* fallback: PIT drives IRQ 0 via 8259 */
    }

    /* 256 KiB RAM disk = 512 sectors of 512 bytes. Useful as a baseline
       check that the block_device_t interface works even when no real
       disk is attached. */
    block_device_t *rd = ramdisk_init(512);
    selftest_block(rd);

    /* Real storage: virtio-blk over PCI. Only present when QEMU is
       launched with -device virtio-blk-pci. virtio_blk_init wires up its
       INTx handler, but with IF=0 the self-test below still completes via
       the polling fallback (no scheduler yet to block on). */
    block_device_t *vblk = virtio_blk_init();
    if (vblk) selftest_block(vblk);
    g_demo_vblk = vblk;

    /* Filesystem end-to-end test on real storage when present (so writes are
       visible in the host vdisk.img), else on the ramdisk. Runs here, before
       irq_enable, so virtio uses its polling path like selftest_block. */
    selftest_fs(vblk ? vblk : rd);

    /* Now take interrupts. From here virtio completions arrive via IRQ
       and the timer drives preemption + sleeper wakeups. */
    irq_enable();

    device_dump();

    /* Hand kmain off to the scheduler as task 0, then create two
       workers and yield until they're done. Each worker prints, then
       busy-waits on the tick counter for ~500 ms — with timer-driven
       preemption the two workers interleave even though they never
       yield voluntarily inside the wait. */
    extern void scheduler_demo(void);
    scheduler_demo();

    /* Status line on screen so the user sees a visible "we're alive" signal. */
    char buf[80];
    /* simple sprintf-ish using kprintf is overkill; format inline */
    const char *ok = "READY. SERIAL: SEE QEMU STDIO";
    fb->draw_text(fb, FB_FONT_W, fb->height - FB_FONT_H * 2,
                  ok, fb_rgb(fb, 0x80, 0xFF, 0x80),
                  fb_rgb(fb, 0x00, 0x00, 0x00));
    (void)buf;

    kprintf("[kernel] init complete, halting\n");
    halt_forever();
}
