#ifndef VIBEOS_FS_H
#define VIBEOS_FS_H

#include <stdint.h>
#include "block.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VibeFS v1 — a tiny, non-journaled, writable filesystem over block_device_t.
 *
 * One volume, 4 KiB FS blocks (8 underlying 512-byte sectors). The on-disk
 * inode table is the source of truth; the two bitmaps are a derived cache that
 * fsck rebuilds from the inodes after an unclean mount. Metadata is written in
 * a crash-safe order (data -> inode -> dirent -> bitmaps+superblock) so a crash
 * leaves at worst orphaned-but-allocated space, never a referenced-but-free
 * block. See kernel/src/fs.c for the gory details.
 */

#define FS_BLOCK_SIZE   4096u
#define FS_MAGIC        0x53464256u    /* 'VBFS' little-endian */
#define FS_VERSION      2u             /* v2: double/triple indirect + 64-bit size */

#define FS_NAME_MAX     60             /* bytes; not counting a NUL */

/* Inode types. 0 means a free inode-table slot. */
#define FT_NONE         0
#define FT_REG          1              /* regular file */
#define FT_DIR          2              /* directory */
#define FT_SYMLINK      3              /* symbolic link (target stored as data) */

/* Block-pointer geometry of an inode.
 *
 *   13 direct          + 1 single-indirect (1024)
 *    + 1 double-indirect (1024^2) + 1 triple-indirect (1024^3)
 *
 * gives a maximum file of ~4 TiB (1024^3 * 4 KiB). The 64-bit `size` field and
 * 64-bit file offsets are what let us actually reach past 4 GiB. */
#define FS_NDIRECT          13
#define FS_NPTR_PER_BLOCK   (FS_BLOCK_SIZE / sizeof(uint32_t))   /* 1024 ptrs/block */
#define FS_NINDIRECT        FS_NPTR_PER_BLOCK                    /* single-indirect reach */
#define FS_NDOUBLE          ((uint64_t)FS_NPTR_PER_BLOCK * FS_NPTR_PER_BLOCK)
#define FS_NTRIPLE          ((uint64_t)FS_NPTR_PER_BLOCK * FS_NPTR_PER_BLOCK * FS_NPTR_PER_BLOCK)
#define FS_MAXFILEBLKS      ((uint64_t)FS_NDIRECT + FS_NINDIRECT + FS_NDOUBLE + FS_NTRIPLE)

/* Bits addressable by one 4 KiB bitmap block. */
#define FS_BITS_PER_BMBLK   (FS_BLOCK_SIZE * 8u)                 /* 32768 */

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
    uint32_t inode_bitmap_blocks;  /* >= 1 */
    uint32_t data_bitmap_blk;      /* first data-bitmap block */
    uint32_t data_bitmap_blocks;   /* >= 1; ceil(data_blocks / 32768) */
    uint32_t inode_table_blk;      /* first inode-table block */
    uint32_t inode_table_blocks;   /* IT */
    uint32_t data_start_blk;       /* first data block */
    uint32_t data_blocks;          /* total_blocks - data_start_blk */
    uint32_t root_inode;           /* = 1 */
    uint32_t dirty;                /* 1 while mounted / after a crash */
    uint32_t mount_tick;           /* timer tick of last mount */
    uint32_t write_tick;           /* timer tick of last metadata flush */
    uint8_t  pad[FS_BLOCK_SIZE - 17 * sizeof(uint32_t)];
} __attribute__((packed)) superblock_t;
static_assert(sizeof(superblock_t) == FS_BLOCK_SIZE, "superblock must be one block");

typedef struct {
    uint16_t type;                 /* FT_NONE / FT_REG / FT_DIR */
    uint16_t links;                /* directory references */
    uint64_t size;                 /* bytes (64-bit: files may exceed 4 GiB) */
    uint32_t ctime;                /* tick at creation */
    uint32_t mtime;                /* tick at last write */
    uint32_t direct[FS_NDIRECT];   /* data block numbers, 0 == hole/none */
    uint32_t indirect;             /* single-indirect: block of 1024 u32 ptrs */
    uint32_t indirect2;            /* double-indirect: block of ptrs to L1 blocks */
    uint32_t indirect3;            /* triple-indirect: block of ptrs to L2 blocks */
    uint32_t mode;                 /* permission bits (low 12); 0 == unset (legacy) */
    uint8_t  pad[40];              /* 20 hdr + 52 direct + 12 indirect + 4 mode = 88; +40 = 128 */
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
int fs_seek  (int fd, uint64_t off);   /* set absolute file offset (may exceed 4 GiB) */
int fs_close (int fd);

int fs_create(const char *path);
int fs_mkdir (const char *path);
int fs_unlink(const char *path);                 /* file or empty directory */

int fs_readdir(const char *path, fs_dirent_t *out, int max);

/* ---- §4 rung 2: inode-level primitives for the per-process fd layer ----
 * These let the syscall layer own the open-file state (offset, kind) instead
 * of the fixed 16-slot global pool fs_open uses, and reach directories (which
 * fs_open refuses) for getdents. Paths are absolute. */

typedef struct {
    uint16_t type;     /* FT_REG / FT_DIR */
    uint16_t links;
    uint64_t size;
    uint32_t ctime, mtime;   /* timer ticks (100 Hz) */
    uint32_t mode;           /* permission bits (low 12); 0 == unset (legacy) */
} fs_stat_t;

int fs_resolve(const char *path);                    /* inode number (>0) or FS errno; follows symlinks */
int fs_lresolve(const char *path);                   /* like fs_resolve but does NOT follow a final symlink */
int fs_istat  (uint32_t ino, fs_stat_t *out);
int fs_chmod  (uint32_t ino, uint32_t mode);                /* set permission bits (low 12) */
int fs_symlink(const char *target, const char *linkpath);   /* create a symlink */
int fs_readlink(uint32_t ino, char *buf, uint32_t bufsz);   /* read a symlink target (no NUL); bytes or FS errno */
int fs_truncate_ino(uint32_t ino);                   /* drop to zero length */
int fs_truncate_to(uint32_t ino, uint64_t len);      /* set length (zero-extends on grow) */
int fs_rename(const char *oldpath, const char *newpath);   /* move file or directory */
int fs_pread  (uint32_t ino, uint64_t off, void *buf, uint32_t n);
int fs_pwrite (uint32_t ino, uint64_t off, const void *buf, uint32_t n);
/* Read one directory entry at byte cursor *pos (advances it). Returns 1 on a
   returned entry, 0 at end, negative on error. */
int fs_dirent_at(uint32_t ino, uint64_t *pos, fs_dirent_t *out);

#ifdef __cplusplus
}
#endif

#endif
