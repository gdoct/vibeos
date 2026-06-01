#include "kernel.h"
#include "io.h"
#include "pmm.h"
#include "paging.h"
#include "pci.h"
#include "task.h"
#include "timer.h"
#include "tty.h"
#include "fb.h"
#include "gui.h"
#include "input.h"

/*
 * UHCI (USB 1.1) host controller + USB HID boot-protocol driver (ROADMAP:
 * USB + input). Keyboard and mouse are low-/full-speed HID devices, so UHCI —
 * a port-I/O controller with a 1024-entry frame list of queue heads and transfer
 * descriptors — is the tractable host controller (vs. MMIO xHCI/EHCI).
 *
 * Bring-up: probe the PIIX3 UHCI (8086:7020), reset it, build the frame list and
 * a small queue-head schedule, reset the root ports, and enumerate each attached
 * device over endpoint 0 (control transfers). For HID devices we switch to the
 * boot protocol and then poll the interrupt-IN endpoint each frame: keyboard
 * reports become console keystrokes (tty_input), mouse reports are reported as
 * input events. Everything runs in a kernel worker ("usbd"); no IRQ wiring —
 * the controller walks the schedule autonomously and we poll descriptor status.
 */

/* ---- UHCI I/O registers (offsets from BAR4 I/O base) ---- */
#define REG_CMD        0x00   /* USBCMD   (16) */
#define REG_STS        0x02   /* USBSTS   (16) */
#define REG_INTR       0x04   /* USBINTR  (16) */
#define REG_FRNUM      0x06   /* FRNUM    (16) */
#define REG_FRBASE     0x08   /* FRBASEADD(32) */
#define REG_SOF        0x0C   /* SOFMOD   (8)  */
#define REG_PORTSC1    0x10   /* PORTSC1  (16) */
#define REG_PORTSC2    0x12   /* PORTSC2  (16) */

#define CMD_RUN        0x0001
#define CMD_HCRESET    0x0002
#define CMD_GRESET     0x0004
#define CMD_MAXP       0x0080

#define PORT_CCS       0x0001  /* current connect status */
#define PORT_CSC       0x0002  /* connect status change (write 1 to clear) */
#define PORT_PED       0x0004  /* port enabled */
#define PORT_PEDC      0x0008  /* enable change (write 1 to clear) */
#define PORT_LS        0x0100  /* low-speed device attached */
#define PORT_RESET     0x0200
#define PORT_SUSPEND   0x1000

/* ---- transfer descriptor / queue head (must live in 32-bit DMA memory) ---- */
#define LINK_TERMINATE 0x1
#define LINK_QH        0x2
#define LINK_DEPTH     0x4

typedef struct {
    volatile uint32_t link;
    volatile uint32_t cs;      /* control + status */
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t pad[4];           /* pad to 32 bytes */
} __attribute__((aligned(16))) uhci_td_t;

typedef struct {
    volatile uint32_t head;    /* horizontal link (next QH) */
    volatile uint32_t element; /* vertical link (to TDs) */
    uint32_t pad[2];
} __attribute__((aligned(16))) uhci_qh_t;

/* TD control/status bits */
#define TD_ACTIVE      (1u << 23)
#define TD_IOC         (1u << 24)
#define TD_LS          (1u << 26)
#define TD_ERRCNT      (3u << 27)
#define TD_STATUS_MASK (0xFFu << 16)
#define TD_ACTLEN(cs)  (((cs) & 0x7FF))

/* TD token PIDs */
#define PID_SETUP      0x2D
#define PID_IN         0x69
#define PID_OUT        0xE1

/* ---- driver state ---- */
static uint16_t  g_io;                 /* UHCI I/O base */
static uint32_t *g_framelist;          /* 1024 dwords */
static uhci_qh_t *g_qh_ctrl;           /* control transfers (enumeration) */
static uhci_qh_t *g_sched_head;        /* first QH the frame list points at */

/* a tiny bump DMA allocator over a few pages */
static uint8_t  *g_dma;
static uint64_t  g_dma_phys;
static uint32_t  g_dma_off, g_dma_cap;

static void *dma_alloc(uint32_t size, uint32_t align, uint64_t *phys_out) {
    g_dma_off = (g_dma_off + align - 1) & ~(align - 1);
    if (g_dma_off + size > g_dma_cap) panic("uhci: DMA pool exhausted");
    void *p = g_dma + g_dma_off;
    if (phys_out) *phys_out = g_dma_phys + g_dma_off;
    g_dma_off += size;
    kmemset(p, 0, size);
    return p;
}
static uint32_t dma_phys32(const void *p) {
    return (uint32_t)(g_dma_phys + ((const uint8_t *)p - g_dma));
}

/* ---- a HID device we found ---- */
typedef struct {
    int      used;
    int      proto;            /* 1 = keyboard, 2 = relative mouse, 0 = abs pointer (tablet) */
    uint8_t  addr, ep, maxpkt, lowspeed, toggle, rlen;
    uhci_qh_t *qh;             /* this endpoint's interrupt QH (in the frame list) */
    uhci_td_t *td;             /* its single interrupt-IN TD */
    uint8_t  *report;          /* DMA report buffer */
    uint64_t  report_phys;
    uint8_t   prev[8];         /* previous keyboard report (edge detection) */
} hid_dev_t;

#define HID_MAX 4
static hid_dev_t g_hid[HID_MAX];

/* ---- control transfer over endpoint 0 ---- */

/* Run a control transfer on endpoint 0 via g_qh_ctrl and wait for it to retire.
   `data`/`len` is the data stage (copied out for OUT, back in for IN). The TDs
   and buffers are bump-allocated and rolled back afterwards (transient). */
static int ctrl_xfer(uint8_t addr, uint8_t maxpkt, int lowspeed,
                     const uint8_t setup[8], uint8_t *data, int len, int in) {
    uint32_t mark = g_dma_off;
    uint32_t ls = lowspeed ? TD_LS : 0;

    uint64_t setph; uint8_t *setbuf = (uint8_t *)dma_alloc(8, 16, &setph);
    kmemcpy(setbuf, setup, 8);
    uint64_t datph = 0; uint8_t *datbuf = nullptr;
    if (len > 0) { datbuf = (uint8_t *)dma_alloc(len, 16, &datph); if (!in && data) kmemcpy(datbuf, data, len); }

    uhci_td_t *s = (uhci_td_t *)dma_alloc(sizeof *s, 16, nullptr);
    s->cs = TD_ACTIVE | TD_ERRCNT | ls;
    s->token = (7u << 21) | (0 << 15) | ((uint32_t)addr << 8) | PID_SETUP;  /* SETUP: 8 bytes, toggle 0 */
    s->buffer = (uint32_t)setph;
    uhci_td_t *prev = s, *last = s;

    int toggle = 1, off = 0;
    while (off < len) {
        int chunk = len - off; if (chunk > maxpkt) chunk = maxpkt;
        uhci_td_t *d = (uhci_td_t *)dma_alloc(sizeof *d, 16, nullptr);
        d->cs = TD_ACTIVE | TD_ERRCNT | ls;
        d->token = (((uint32_t)(chunk - 1) & 0x7FF) << 21) | ((uint32_t)toggle << 19) |
                   ((uint32_t)addr << 8) | (in ? PID_IN : PID_OUT);
        d->buffer = (uint32_t)(datph + off);
        prev->link = dma_phys32(d) | LINK_DEPTH;
        prev = last = d;
        toggle ^= 1; off += chunk;
    }

    uhci_td_t *st = (uhci_td_t *)dma_alloc(sizeof *st, 16, nullptr);
    st->cs = TD_ACTIVE | TD_ERRCNT | ls | TD_IOC;
    st->token = (0x7FFu << 21) | (1u << 19) | ((uint32_t)addr << 8) |
                (in ? PID_OUT : PID_IN);     /* status: opposite dir, zero length, toggle 1 */
    st->buffer = 0;
    st->link = LINK_TERMINATE;
    prev->link = dma_phys32(st) | LINK_DEPTH;
    last = st;

    g_qh_ctrl->element = dma_phys32(s);
    int ok = -1;
    for (int i = 0; i < 1000; i++) {
        if (!(last->cs & TD_ACTIVE)) { ok = ((last->cs & TD_STATUS_MASK) == 0) ? 0 : -1; break; }
        ksleep_ms(1);
    }
    g_qh_ctrl->element = LINK_TERMINATE;

    if (ok == 0 && in && data && datbuf) kmemcpy(data, datbuf, len);
    g_dma_off = mark;                            /* free the transient TDs/buffers */
    return ok;
}

static int get_descriptor(uint8_t addr, uint8_t maxpkt, int ls, int type, int index,
                          uint8_t *buf, int len) {
    uint8_t setup[8] = { 0x80, 0x06, (uint8_t)index, (uint8_t)type, 0, 0,
                         (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return ctrl_xfer(addr, maxpkt, ls, setup, buf, len, 1);
}
static int set_address(uint8_t maxpkt, int ls, int newaddr) {
    uint8_t setup[8] = { 0x00, 0x05, (uint8_t)newaddr, 0, 0, 0, 0, 0 };
    return ctrl_xfer(0, maxpkt, ls, setup, nullptr, 0, 0);
}
static int set_config(uint8_t addr, uint8_t maxpkt, int ls, int cfg) {
    uint8_t setup[8] = { 0x00, 0x09, (uint8_t)cfg, 0, 0, 0, 0, 0 };
    return ctrl_xfer(addr, maxpkt, ls, setup, nullptr, 0, 0);
}
static int hid_set(uint8_t addr, uint8_t maxpkt, int ls, int req, int value, int iface) {
    uint8_t setup[8] = { 0x21, (uint8_t)req, (uint8_t)value, (uint8_t)(value >> 8),
                         (uint8_t)iface, 0, 0, 0 };
    return ctrl_xfer(addr, maxpkt, ls, setup, nullptr, 0, 0);
}

/* ---- HID input ---- */

/* US keyboard boot-protocol scancode -> ASCII (unshifted / shifted). */
static const char kmap[2][0x40] = {
  { /* unshifted, index = HID usage 0x00..0x3F */
    0,0,0,0, 'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
    'q','r','s','t','u','v','w','x','y','z','1','2','3','4','5','6','7','8','9','0',
    '\n',27,'\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/',0,0,0 },
  { /* shifted */
    0,0,0,0, 'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z','!','@','#','$','%','^','&','*','(',')',
    '\n',27,'\b','\t',' ','_','+','{','}','|',0,':','"','~','<','>','?',0,0,0 },
};

static void kbd_report(hid_dev_t *h, const uint8_t *r) {
    int shift = (r[0] & 0x22) != 0;            /* L/R shift modifiers */
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k == 0) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (h->prev[j] == k) { was = 1; break; }
        if (was) continue;                     /* still held: not a fresh press */
        if (k < 0x40 && kmap[shift][k]) {
            char c = kmap[shift][k];
            if (input_grabbed())      input_push_key(c); /* userspace GUI server */
            else if (gui_wants_keyboard()) gui_input_key(c);  /* in-kernel WM textbox */
            else                      tty_input(c);      /* otherwise the console */
        }
    }
    kmemcpy(h->prev, r, 8);
}

/* Accumulated mouse state, exported for a future GUI (usb_mouse_get). The
   pointer is clamped to a virtual screen; a GUI consumer can rebind the bounds. */
static volatile int g_mx = 512, g_my = 384, g_mbtn, g_last_btn;

static void mouse_report(const uint8_t *r) {
    int8_t dx = (int8_t)r[1], dy = (int8_t)r[2];
    g_mx += dx; g_my += dy;
    int mw = 1024, mh = 768;                     /* clamp to the framebuffer, if any */
    fb_device_t *fb = fb_get();
    if (fb) { mw = (int)fb->width; mh = (int)fb->height; }
    if (g_mx < 0) g_mx = 0; else if (g_mx >= mw) g_mx = mw - 1;
    if (g_my < 0) g_my = 0; else if (g_my >= mh) g_my = mh - 1;
    int btn = r[0] & 7;
    g_mbtn = btn;
    /* Feed the userspace GUI server's input ring on any motion or button change. */
    if (input_grabbed() && (dx || dy || btn != g_last_btn))
        input_push_mouse(g_mx, g_my, btn);
    g_last_btn = btn;
}

/* Absolute pointer (USB tablet) boot report: byte0 = buttons, bytes 1..2 = X,
   bytes 3..4 = Y (little-endian, logical range 0..0x7FFF), byte5 = wheel. Unlike
   a relative mouse there is no accumulation or grab — the host cursor position is
   delivered directly, which is why a tablet "just works" in a VM. We rescale the
   0..0x7FFF axes onto the framebuffer. */
static void tablet_report(const uint8_t *r) {
    int ax = r[1] | (r[2] << 8);
    int ay = r[3] | (r[4] << 8);
    int mw = 1024, mh = 768;
    fb_device_t *fb = fb_get();
    if (fb) { mw = (int)fb->width; mh = (int)fb->height; }
    g_mx = (int)((int64_t)ax * mw / 0x8000);
    g_my = (int)((int64_t)ay * mh / 0x8000);
    if (g_mx >= mw) g_mx = mw - 1;
    if (g_my >= mh) g_my = mh - 1;
    int btn = r[0] & 7;
    g_mbtn = btn;
    if (input_grabbed()) input_push_mouse(g_mx, g_my, btn);
    g_last_btn = btn;
}

/* Current pointer position + button bitmask (bit0=L,1=R,2=M). For the GUI. */
extern "C" void usb_mouse_get(int *x, int *y, int *buttons) {
    if (x) *x = g_mx; if (y) *y = g_my; if (buttons) *buttons = g_mbtn;
}

/* Arm a HID device's interrupt-IN TD (re-used each poll). */
static void hid_arm(hid_dev_t *h) {
    int rlen = h->rlen;
    h->td->cs = TD_ACTIVE | TD_ERRCNT | (h->lowspeed ? TD_LS : 0);
    h->td->token = (((uint32_t)(rlen - 1) & 0x7FF) << 21) | ((uint32_t)h->toggle << 19) |
                   ((uint32_t)h->ep << 15) | ((uint32_t)h->addr << 8) | PID_IN;
    h->td->buffer = (uint32_t)h->report_phys;
    h->td->link = LINK_TERMINATE;
    h->qh->element = dma_phys32(h->td);
}

static void hid_poll(hid_dev_t *h) {
    if (h->td->cs & TD_ACTIVE) return;          /* NAK / not ready yet */
    uint32_t cs = h->td->cs;
    if ((cs & TD_STATUS_MASK) == 0) {           /* success: a report arrived */
        if      (h->proto == 1) kbd_report(h, h->report);     /* keyboard */
        else if (h->proto == 2) mouse_report(h->report);      /* relative mouse */
        else                    tablet_report(h->report);     /* absolute pointer */
    }
    h->toggle ^= 1;
    hid_arm(h);                                 /* re-poll */
}

/* ---- enumeration ---- */

static hid_dev_t *hid_alloc(void) {
    for (int i = 0; i < HID_MAX; i++) if (!g_hid[i].used) return &g_hid[i];
    return nullptr;
}

static int g_next_addr = 1;

static void enumerate_port(int port) {
    uint16_t reg = port == 0 ? REG_PORTSC1 : REG_PORTSC2;
    uint16_t sc = inw(g_io + reg);
    if (!(sc & PORT_CCS)) return;               /* nothing attached */

    /* reset + enable the port */
    outw(g_io + reg, PORT_RESET);
    ksleep_ms(60);
    outw(g_io + reg, inw(g_io + reg) & ~PORT_RESET);
    ksleep_ms(5);
    for (int i = 0; i < 10; i++) {              /* enable, clearing change bits */
        outw(g_io + reg, PORT_PED | PORT_CSC | PORT_PEDC);
        ksleep_ms(5);
        if (inw(g_io + reg) & PORT_PED) break;
    }
    sc = inw(g_io + reg);
    if (!(sc & PORT_PED)) { kprintf("[usb] port %d: failed to enable\n", port + 1); return; }
    int ls = (sc & PORT_LS) ? 1 : 0;

    uint8_t buf[64];
    uint8_t maxpkt = 8;
    if (get_descriptor(0, maxpkt, ls, 1, 0, buf, 8) != 0) { kprintf("[usb] port %d: no device descriptor\n", port + 1); return; }
    maxpkt = buf[7] ? buf[7] : 8;               /* bMaxPacketSize0 */

    int addr = g_next_addr++;
    if (set_address(maxpkt, ls, addr) != 0) { kprintf("[usb] port %d: SET_ADDRESS failed\n", port + 1); return; }
    ksleep_ms(5);

    if (get_descriptor(addr, maxpkt, ls, 1, 0, buf, 18) != 0) return;
    uint16_t vid = buf[8] | (buf[9] << 8), pid = buf[10] | (buf[11] << 8);

    /* config descriptor: read 9, then the full thing */
    if (get_descriptor(addr, maxpkt, ls, 2, 0, buf, 9) != 0) return;
    int total = buf[2] | (buf[3] << 8);
    if (total > (int)sizeof buf) total = sizeof buf;
    if (get_descriptor(addr, maxpkt, ls, 2, 0, buf, total) != 0) return;
    int cfgval = buf[5];

    /* walk interface + endpoint descriptors */
    int proto = 0, iface = 0, ep = 0, eppkt = 8, is_hid = 0, subclass = 0;
    for (int o = 0; o + 2 <= total; ) {
        int blen = buf[o], btype = buf[o + 1];
        if (blen == 0) break;
        if (btype == 0x04) {                    /* interface */
            if (buf[o + 5] == 0x03) {           /* bInterfaceClass = HID */
                is_hid = 1;
                subclass = buf[o + 6];          /* bInterfaceSubClass: 1=boot */
                proto = buf[o + 7];             /* bInterfaceProtocol: 1=kbd 2=mouse */
                iface = buf[o + 2];
            } else is_hid = 0;
        } else if (btype == 0x05 && is_hid) {   /* endpoint */
            if ((buf[o + 2] & 0x80) && (buf[o + 3] & 0x03) == 0x03) {  /* IN interrupt */
                ep = buf[o + 2] & 0x0F;
                eppkt = buf[o + 4];
            }
        }
        o += blen;
    }
    if (!is_hid || !ep) { kprintf("[usb] port %d: %04x:%04x not a HID device\n", port + 1, vid, pid); return; }

    set_config(addr, maxpkt, ls, cfgval);
    if (subclass == 1)                          /* boot-protocol device only */
        hid_set(addr, maxpkt, ls, 0x0B, 0, iface);  /* SET_PROTOCOL: boot */
    hid_set(addr, maxpkt, ls, 0x0A, 0, iface);  /* SET_IDLE: 0 (report on change) */

    hid_dev_t *h = hid_alloc();
    if (!h) return;
    h->used = 1; h->proto = proto; h->addr = addr; h->ep = ep;
    h->rlen = proto == 1 ? 8 : (uint8_t)(eppkt ? eppkt : 4);
    h->maxpkt = eppkt ? eppkt : 8; h->lowspeed = ls; h->toggle = 0;
    uint64_t rp; h->report = (uint8_t *)dma_alloc(16, 16, &rp); h->report_phys = rp;
    h->qh = (uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), 16, nullptr);
    h->td = (uhci_td_t *)dma_alloc(sizeof(uhci_td_t), 16, nullptr);
    /* splice this endpoint's QH at the head of the schedule (chains to whatever
       was first — other HID QHs, then the control QH). */
    h->qh->head = dma_phys32(g_sched_head) | LINK_QH;
    h->qh->element = LINK_TERMINATE;
    g_sched_head = h->qh;
    for (uint32_t i = 0; i < 1024; i++) g_framelist[i] = dma_phys32(g_sched_head) | LINK_QH;
    hid_arm(h);

    const char *kind = proto == 1 ? "keyboard" : proto == 2 ? "mouse" : "tablet";
    kprintf("[usb] port %d: %s %04x:%04x addr %d ep %d (%s-speed) -> ready\n",
            port + 1, kind, vid, pid, addr, ep, ls ? "low" : "full");
}

/* ---- controller bring-up + worker ---- */

static void usb_worker(void *arg) {
    (void)arg;
    ksleep_ms(100);

    /* reset the host controller */
    outw(g_io + REG_CMD, CMD_HCRESET);
    for (int i = 0; i < 50 && (inw(g_io + REG_CMD) & CMD_HCRESET); i++) ksleep_ms(1);
    outw(g_io + REG_INTR, 0);                   /* no interrupts: we poll */
    outw(g_io + REG_FRNUM, 0);
    outl(g_io + REG_FRBASE, (uint32_t)g_dma_phys);   /* frame list is at pool base */
    outw(g_io + REG_SOF, 0x40);
    outw(g_io + REG_STS, 0xFFFF);               /* clear status */
    outw(g_io + REG_CMD, CMD_RUN | CMD_MAXP);   /* run, 64-byte max packet */

    ksleep_ms(50);
    enumerate_port(0);
    enumerate_port(1);

    int n = 0; for (int i = 0; i < HID_MAX; i++) if (g_hid[i].used) n++;
    if (!n) { kprintf("[usb] no HID devices; worker idle\n"); }

    for (;;) {                                  /* poll HID endpoints ~125 Hz */
        for (int i = 0; i < HID_MAX; i++) if (g_hid[i].used) hid_poll(&g_hid[i]);
        ksleep_ms(8);
    }
}

extern "C" void usb_init(void) {
    pci_dev_t d;
    if (!pci_find(0x8086, 0x7020, 0x7020, &d)) {   /* PIIX3 UHCI */
        kprintf("[usb] no UHCI controller\n");
        return;
    }
    uint16_t cmd = pci_read16(d.bus, d.dev, d.fn, PCI_COMMAND);
    pci_write16(d.bus, d.dev, d.fn, PCI_COMMAND, cmd | PCI_CMD_IO | PCI_CMD_BUS_MASTER);
    uint32_t bar4 = pci_read32(d.bus, d.dev, d.fn, PCI_BAR0 + 16);
    g_io = (uint16_t)(bar4 & ~0x3u);

    /* DMA pool: page 0 = frame list (1024 dwords), the rest = QHs/TDs/buffers. */
    uint64_t phys = pmm_alloc_pages(8);
    if (!phys) { kprintf("[usb] DMA alloc failed\n"); return; }
    g_dma = (uint8_t *)phys_to_virt(phys);
    g_dma_phys = phys;
    g_dma_cap = 8 * PAGE_SIZE;
    kmemset(g_dma, 0, g_dma_cap);

    g_framelist = (uint32_t *)dma_alloc(1024 * sizeof(uint32_t), PAGE_SIZE, nullptr);
    g_qh_ctrl = (uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), 16, nullptr);
    g_qh_ctrl->head = LINK_TERMINATE;
    g_qh_ctrl->element = LINK_TERMINATE;
    g_sched_head = g_qh_ctrl;
    for (uint32_t i = 0; i < 1024; i++) g_framelist[i] = dma_phys32(g_qh_ctrl) | LINK_QH;

    kprintf("[usb] UHCI at I/O 0x%x; starting usbd worker\n", g_io);
    task_create("usbd", usb_worker, nullptr);
}
