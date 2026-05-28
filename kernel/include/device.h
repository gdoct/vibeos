#ifndef MYOS_DEVICE_H
#define MYOS_DEVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Devices are plain structs containing a header (this device_t) followed by
 * class-specific fields and ops. Drivers register their device at init time;
 * the kernel can then look them up by class and iterate.
 *
 * Why this shape: it keeps the registry trivial (a linked list of device_t*)
 * while letting each driver embed its private state inline. No ad-hoc void*
 * casts in the registry itself; users downcast container-of style.
 */

typedef enum {
    DEV_NONE        = 0,
    DEV_FRAMEBUFFER = 1,
    DEV_BLOCK       = 2,
    DEV_CHAR        = 3,
} dev_class_t;

typedef struct device {
    const char       *name;
    dev_class_t       cls;
    struct device    *next;   /* intrusive; managed by device_register */
} device_t;

void      device_register(device_t *dev);
device_t *device_first(dev_class_t cls);
device_t *device_next(device_t *dev, dev_class_t cls);
device_t *device_find(dev_class_t cls, const char *name);
void      device_dump(void);

#ifdef __cplusplus
}
#endif

#endif
