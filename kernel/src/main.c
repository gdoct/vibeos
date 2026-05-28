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

void scheduler_demo(void) {
    sched_init();
    g_alive_workers = 2;
    task_create("w0", demo_worker, (void *)(intptr_t)0);
    task_create("w1", demo_worker, (void *)(intptr_t)1);
    if (g_demo_vblk) {
        g_alive_workers++;
        task_create("io", io_worker, nullptr);
    }
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
    serial_init();
    kprintf("\n[kernel] MyOS booting (BootInfo @ %p)\n", bi);

    gdt_init();
    idt_init();

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
    selftest_heap();

    fb_device_t *fb = fb_init(&bi->fb);
    paint_splash(fb);

    /* PIC remap + 100 Hz tick source. Done before device init so a driver
       can register and unmask its IRQ line and have it survive the remap.
       Interrupts stay globally masked (IF=0) until irq_enable() below. */
    irq_init();
    timer_init(100);

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
