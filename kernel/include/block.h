#ifndef VIBEOS_BLOCK_H
#define VIBEOS_BLOCK_H

#include <stdint.h>
#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Block device contract: fixed-size blocks addressed by LBA. read/write
 * return 0 on success or a negative errno-style code. Drivers may transfer
 * any number of contiguous blocks per call but are not required to be async.
 */

#define BLK_OK           0
#define BLK_ERR_IO      -1
#define BLK_ERR_RANGE   -2
#define BLK_ERR_NOSYS   -3

typedef struct block_device {
    device_t  dev;
    uint32_t  block_size;     /* bytes; typically 512 */
    uint64_t  num_blocks;     /* total addressable LBAs */

    int (*read) (struct block_device *, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *, uint64_t lba, uint32_t count, const void *buf);
} block_device_t;

/* Convenience helpers — perform bounds checks and dispatch. */
int block_read (block_device_t *bd, uint64_t lba, uint32_t count, void *buf);
int block_write(block_device_t *bd, uint64_t lba, uint32_t count, const void *buf);

#ifdef __cplusplus
}
#endif

#endif
