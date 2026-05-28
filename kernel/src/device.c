#include "kernel.h"
#include "device.h"

static device_t *g_devs = nullptr;

static const char *cls_name(dev_class_t cls) {
    switch (cls) {
    case DEV_FRAMEBUFFER: return "framebuffer";
    case DEV_BLOCK:       return "block";
    case DEV_CHAR:        return "char";
    default:              return "none";
    }
}

void device_register(device_t *dev) {
    dev->next = g_devs;
    g_devs = dev;
    kprintf("[dev] register %s (%s)\n", dev->name ? dev->name : "?", cls_name(dev->cls));
}

device_t *device_first(dev_class_t cls) {
    for (device_t *d = g_devs; d; d = d->next)
        if (d->cls == cls) return d;
    return nullptr;
}

device_t *device_next(device_t *dev, dev_class_t cls) {
    if (!dev) return nullptr;
    for (device_t *d = dev->next; d; d = d->next)
        if (d->cls == cls) return d;
    return nullptr;
}

device_t *device_find(dev_class_t cls, const char *name) {
    for (device_t *d = g_devs; d; d = d->next) {
        if (d->cls != cls) continue;
        if (!d->name || !name) continue;
        const char *a = d->name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return d;
    }
    return nullptr;
}

void device_dump(void) {
    kprintf("[dev] registered devices:\n");
    for (device_t *d = g_devs; d; d = d->next)
        kprintf("[dev]   %-12s %s\n", cls_name(d->cls), d->name ? d->name : "?");
}
