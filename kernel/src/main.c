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
#include "smp.h"
#include "fs.h"
#include "net.h"
#include "csprng.h"
#include "config.h"
#include "usermode.h"
#include "percpu.h"
#include "../../boot/include/bootinfo.h"

extern "C" block_device_t *ramdisk_init(uint64_t num_blocks);
extern "C" block_device_t *virtio_blk_init(void);
extern "C" int virtio_rng_seed(void);
extern "C" void usb_init(void);

/* Load /bin/init from the mounted root filesystem and drop to ring 3. Runs as
   a task, so it owns a kernel stack for syscalls/IRQs; on exit() it dies and
   the boot task halts. The program image comes off disk (user_load_path reads
   it from VibeFS) — no embedded blob. */
static void init_launch(void *arg) {
    (void)arg;
    /* init gets its own address space (low half private, kernel half shared). */
    vmspace_t *vm = vmspace_create();
    if (!vm) panic("init: vmspace_create failed");
    task_set_vmspace(vm);             /* attach + make active for the load below */

    uint64_t entry, rsp;
    char *argv[] = { (char *)"/bin/init", nullptr };
    char *envp[] = { (char *)"PATH=/bin", (char *)"TERM=vibeos", nullptr };
    int r = user_load_path(vm, "/bin/init", argv, envp, &entry, &rsp);
    if (r != 0) panic("init: failed to load /bin/init (%d) — is the volume populated?", r);
    kprintf("[init] /bin/init loaded, entry=%lx rsp=%lx -> ring 3\n",
            (unsigned long)entry, (unsigned long)rsp);
    enter_user(entry, rsp);
}

/* A kernel demo task: prints which CPU it's running on a few times, then
   exits. With several of these plus SMP, the scheduler spreads them across
   CPUs (and they migrate as they sleep/wake) — a visible sign of stage B. */
static void smp_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 6; i++) {
        kprintf("[worker %d] iter %d on CPU %d\n", id, i, smp_cpu_index());
        ksleep_ms(250);
    }
}

/* Create the initial task set: /bin/init (the user shell, pinned to the BSP)
   plus a few kernel workers that exercise multi-CPU scheduling. */
static void create_initial_tasks(void) {
    /* init becomes a user task inside init_launch; pin it to the BSP up front so
       no AP steals it during the window before it sets its address space (§3).
       Safe to set here: the APs are not scheduling yet. */
    task_t *it = task_create("init", init_launch, nullptr);
    it->bsp_only = 1;
    for (int i = 0; i < 4; i++)
        task_create("worker", smp_worker, (void *)(intptr_t)i);
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
                  "VibeOS kernel", white, teal);
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
    kprintf("\n[kernel] VibeOS booting (BootInfo @ %p)\n", bi);

    gdt_init();
    idt_init();
    percpu_init(0);    /* BSP per-CPU TSS + GS base (ring-3 -> ring-0 stack) */
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
    page_refcount_init();   /* COW/usercopy page-refcount table (ROADMAP §1.1) */
    csprng_init();          /* ChaCha20 DRBG: TCP ISNs etc. (ROADMAP §2) */
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
       INTx handler. */
    block_device_t *vblk = virtio_blk_init();

    /* Fold hardware entropy from virtio-rng into the CSPRNG, if present
       (-device virtio-rng-pci). Polled; no IRQ. Best-effort (ROADMAP §2). */
    virtio_rng_seed();

    /* Mount the root volume (virtio-blk if present, else the ramdisk) and keep
       it mounted for the lifetime of the system — userspace programs are loaded
       from it (/bin/init). Runs before irq_enable, so virtio uses its polling
       path. The volume is expected to be a populated VibeFS image (built by
       build.sh via diskutil-cli); mkfs runs only if it is unformatted. */
    block_device_t *root_dev = vblk ? vblk : rd;
    int mr = fs_mount(root_dev);
    if (mr != FS_OK) panic("fs: failed to mount root volume (%d)", mr);
    kprintf("[fs] root volume '%s' mounted\n", root_dev->dev.name);

    /* System configuration (ROADMAP: config service). Read /config/system.conf
       now that the fs is mounted, so subsystems below can consult it. */
    config_init();
    { const char *motd = config_get("motd"); if (motd) kprintf("[motd] %s\n", motd); }

    /* Now take interrupts. From here virtio completions arrive via IRQ
       and the timer drives preemption + sleeper wakeups. */
    irq_enable();

    /* SMP scheduler bring-up. Initialize it, create the initial tasks, then
       bring up the other CPUs (each joins the scheduler from ap_entry). The
       initial tasks exist first so the APs have work the moment they're up. */
    sched_init();
    net_init();           /* probe virtio-net + start the net worker (ROADMAP §5) */
    usb_init();           /* probe UHCI + start the USB HID worker (keyboard/mouse) */
    create_initial_tasks();
    smp_init();
    smp_ipi_selftest();   /* verify the cross-CPU IPI path (ROADMAP §2) */

    device_dump();

    /* Visible "we're alive" banner before this CPU dissolves into the
       scheduler loop. */
    fb->draw_text(fb, FB_FONT_W, fb->height - FB_FONT_H * 2,
                  "READY. SERIAL: SEE QEMU STDIO", fb_rgb(fb, 0x80, 0xFF, 0x80),
                  fb_rgb(fb, 0x00, 0x00, 0x00));

    kprintf("[kernel] BSP entering scheduler\n");
    scheduler();   /* never returns */
}
