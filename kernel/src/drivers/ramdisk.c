#include "kernel.h"
#include "pmm.h"
#include "paging.h"   /* PMM storage is reached via the direct map */
#include "block.h"

/*
 * RAM-backed block device. Exists primarily to exercise the block_device_t
 * interface end-to-end before a real disk driver is wired up. Storage is
 * allocated from the PMM at init time.
 */

#define RAMDISK_BLOCK_SIZE 512

typedef struct {
    block_device_t  bd;
    uint8_t        *storage;     /* contiguous block_size * num_blocks bytes */
} ramdisk_t;

static ramdisk_t g_rd;

static int rd_read(block_device_t *bd, uint64_t lba, uint32_t count, void *buf) {
    ramdisk_t *rd = (ramdisk_t *)bd;
    if (lba + count > bd->num_blocks) return BLK_ERR_RANGE;
    kmemcpy(buf, rd->storage + lba * bd->block_size,
            (size_t)count * bd->block_size);
    return BLK_OK;
}

static int rd_write(block_device_t *bd, uint64_t lba, uint32_t count, const void *buf) {
    ramdisk_t *rd = (ramdisk_t *)bd;
    if (lba + count > bd->num_blocks) return BLK_ERR_RANGE;
    kmemcpy(rd->storage + lba * bd->block_size, buf,
            (size_t)count * bd->block_size);
    return BLK_OK;
}

extern "C" block_device_t *ramdisk_init(uint64_t num_blocks) {
    uint64_t bytes = num_blocks * RAMDISK_BLOCK_SIZE;
    size_t pages = (size_t)PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) panic("ramdisk: out of memory (%lu pages)", (unsigned long)pages);

    g_rd.storage          = (uint8_t *)phys_to_virt(phys);
    g_rd.bd.dev.name      = "ram0";
    g_rd.bd.dev.cls       = DEV_BLOCK;
    g_rd.bd.block_size    = RAMDISK_BLOCK_SIZE;
    g_rd.bd.num_blocks    = num_blocks;
    g_rd.bd.read          = rd_read;
    g_rd.bd.write         = rd_write;

    device_register(&g_rd.bd.dev);
    return &g_rd.bd;
}

/* Shared block_device_t convenience helpers. Live here so the block subsystem
   has at least one implementation linked in. */
int block_read(block_device_t *bd, uint64_t lba, uint32_t count, void *buf) {
    if (!bd || !bd->read) return BLK_ERR_NOSYS;
    return bd->read(bd, lba, count, buf);
}

int block_write(block_device_t *bd, uint64_t lba, uint32_t count, const void *buf) {
    if (!bd || !bd->write) return BLK_ERR_NOSYS;
    return bd->write(bd, lba, count, buf);
}
