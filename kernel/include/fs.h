#ifndef MYOS_FS_H
#define MYOS_FS_H

#include <stdint.h>
#include "block.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MyFS v1 — a tiny, non-journaled, writable filesystem over block_device_t.
 *
 * One volume, 4 KiB FS blocks (8 underlying 512-byte sectors). The on-disk
 * inode table is the source of truth; the two bitmaps are a derived cache that
 * fsck rebuilds from the inodes after an unclean mount. Metadata is written in
 * a crash-safe order (data -> inode -> dirent -> bitmaps+superblock) so a crash
 * leaves at worst orphaned-but-allocated space, never a referenced-but-free
 * block. See kernel/src/fs.c for the gory details.
 */

#define FS_BLOCK_SIZE   4096u
#define FS_MAGIC        0x4D594653u    /* 'MYFS' little-endian */
#define FS_VERSION      1u

#define FS_NAME_MAX     60             /* bytes; not counting a NUL */

/* Inode types. 0 means a free inode-table slot. */
#define FT_NONE         0
#define FT_REG          1              /* regular file */
#define FT_DIR          2              /* directory */

/* Block-pointer geometry of an inode. */
#define FS_NDIRECT      13
#define FS_NINDIRECT    (FS_BLOCK_SIZE / sizeof(uint32_t))   /* 1024 */
#define FS_MAXFILEBLKS  (FS_NDIRECT + FS_NINDIRECT)

/* open() flags. */
#define FS_O_CREATE     0x1
#define FS_O_TRUNC      0x2

/* Errno-style return codes (0 == success, negatives are errors). */
#define FS_OK            0
#define FS_EIO          -1
#define FS_ENOENT       -2
#define FS_EEXIST       -3
#define FS_ENOTDIR      -4
#define FS_EISDIR       -5
#define FS_ENOSPC       -6
#define FS_EINVAL       -7
#define FS_ENOTEMPTY    -8
#define FS_EMFILE       -9             /* too many open files */
#define FS_EBADF        -10
#define FS_ENAMETOOLONG -11
#define FS_EFBIG        -12

/* ---- On-disk structures (little-endian, x86). ---- */

typedef struct {
    uint32_t magic;                /* FS_MAGIC */
    uint32_t version;              /* FS_VERSION */
    uint32_t block_size;           /* FS_BLOCK_SIZE */
    uint32_t total_blocks;         /* FS blocks in the volume */
    uint32_t inode_count;          /* total inode slots (incl. 0 and root) */
    uint32_t inode_bitmap_blk;     /* = 1 */
    uint32_t data_bitmap_blk;      /* = 2 */
    uint32_t inode_table_blk;      /* = 3 */
    uint32_t inode_table_blocks;   /* IT */
    uint32_t data_start_blk;       /* 3 + IT */
    uint32_t data_blocks;          /* total_blocks - data_start_blk */
    uint32_t root_inode;           /* = 1 */
    uint32_t dirty;                /* 1 while mounted / after a crash */
    uint32_t mount_tick;           /* timer tick of last mount */
    uint32_t write_tick;           /* timer tick of last metadata flush */
    uint8_t  pad[FS_BLOCK_SIZE - 15 * sizeof(uint32_t)];
} __attribute__((packed)) superblock_t;
static_assert(sizeof(superblock_t) == FS_BLOCK_SIZE, "superblock must be one block");

typedef struct {
    uint16_t type;                 /* FT_NONE / FT_REG / FT_DIR */
    uint16_t links;                /* directory references */
    uint32_t size;                 /* bytes */
    uint32_t ctime;                /* tick at creation */
    uint32_t mtime;                /* tick at last write */
    uint32_t direct[FS_NDIRECT];   /* data block numbers, 0 == hole/none */
    uint32_t indirect;             /* block of FS_NINDIRECT u32 ptrs, 0 == none */
    uint8_t  pad[56];              /* 16 hdr + 52 direct + 4 indirect = 72; +56 = 128 */
} __attribute__((packed)) inode_t;
static_assert(sizeof(inode_t) == 128, "inode must be 128 bytes");

#define FS_INODES_PER_BLOCK   (FS_BLOCK_SIZE / sizeof(inode_t))   /* 32 */

/* Directory entry header; `name` (name_len bytes) follows inline. Records are
   4-byte aligned and never span a block. inode == 0 marks a free slot. */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;              /* total record bytes incl. this header */
    uint8_t  name_len;
    uint8_t  type;                 /* FT_REG / FT_DIR */
    /* char name[name_len] follows */
} __attribute__((packed)) dirent_disk_t;

#define FS_DIRENT_HDR   ((uint32_t)sizeof(dirent_disk_t))   /* 8 */

/* ---- Public API (returned by fs_readdir). ---- */

typedef struct {
    char     name[FS_NAME_MAX + 1];
    uint32_t inode;
    uint8_t  type;
} fs_dirent_t;

/* ---- Volume + file API. All paths are absolute ("/a/b"). ---- */

int fs_mount(block_device_t *dev);   /* mkfs if unformatted; fsck if dirty */
int fs_unmount(void);

int fs_open  (const char *path, int flags);
int fs_read  (int fd, void *buf, uint32_t n);
int fs_write (int fd, const void *buf, uint32_t n);
int fs_close (int fd);

int fs_create(const char *path);
int fs_mkdir (const char *path);
int fs_unlink(const char *path);                 /* file or empty directory */

int fs_readdir(const char *path, fs_dirent_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif
