#include "kernel.h"
#include "fs.h"
#include "block.h"
#include "kmalloc.h"
#include "timer.h"

/*
 * VibeFS v1 — a tiny, non-journaled, writable filesystem (ROADMAP §2).
 *
 * Design in one breath: 4 KiB FS blocks over a 512-byte-sector block device;
 * a superblock, an inode bitmap, a data bitmap, a fixed inode table, then the
 * data area. The inode table is the SOURCE OF TRUTH — the two bitmaps are a
 * derived cache that fsck reconstructs from the inodes after an unclean mount.
 * That choice is what makes the no-journal scheme safe: every mutating op
 * writes in the order data -> inode -> directory entry -> bitmaps+superblock,
 * so a crash mid-op leaves at worst an allocated-but-unreferenced block (an
 * orphan fsck reclaims), never a referenced-but-free block (real corruption).
 *
 * Single volume, no locking — every caller today runs single-threaded on the
 * boot path. Revisit when SMP lands.
 */

/* ---------------------------------------------------------------- mount state */

static struct {
    block_device_t *dev;
    uint32_t        spb;        /* underlying sectors per 4 KiB FS block */
    superblock_t    sb;         /* cached superblock (block 0) */
    uint8_t         inode_bm[FS_BLOCK_SIZE];   /* cached inode bitmap (1 block; <=32768 inodes) */
    uint8_t        *data_bm;    /* cached data bitmap, data_bitmap_blocks * 4 KiB */
    uint8_t        *data_bm_dirty;             /* per data-bitmap-block "needs write" flag */
    uint32_t        data_bm_blocks;            /* == sb.data_bitmap_blocks */
    int             inode_bm_dirty;            /* inode bitmap changed since last flush */
    uint32_t        next_data;  /* allocation cursor: data-block index to try next */
    int             mounted;
} g;

/* A permanently-zero block used as the source when zeroing a disk block.
   Never written to, so it is safe to use re-entrantly. */
static uint8_t g_zeros[FS_BLOCK_SIZE];

#define FS_MAX_OPEN 16
static struct { int used; uint32_t ino; uint64_t off; } g_fds[FS_MAX_OPEN];

/* ---- forward declarations (definitions are grouped by concern below) ---- */
static void     inode_read (uint32_t ino, inode_t *out);
static void     inode_write(uint32_t ino, const inode_t *in);
static uint32_t bmap(inode_t *in, uint32_t fbn, int alloc);
static void     blocks_free(inode_t *in);
static void     mkfs(void);
static void     fsck(void);

/* --------------------------------------------------------------- block layer */

static void bread(uint32_t blk, void *buf) {
    int r = block_read(g.dev, (uint64_t)blk * g.spb, g.spb, buf);
    if (r != BLK_OK) panic("fs: read block %u failed (%d)", blk, r);
}

static void bwrite(uint32_t blk, const void *buf) {
    int r = block_write(g.dev, (uint64_t)blk * g.spb, g.spb, buf);
    if (r != BLK_OK) panic("fs: write block %u failed (%d)", blk, r);
}

static void bzero(uint32_t blk) { bwrite(blk, g_zeros); }

/* ------------------------------------------------------------------- bitmaps */

static int  bm_test(const uint8_t *bm, uint32_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static void bm_set (uint8_t *bm, uint32_t i) { bm[i >> 3] |=  (uint8_t)(1u << (i & 7)); }
static void bm_clr (uint8_t *bm, uint32_t i) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

/* Note the data-bitmap block holding data-block index `d` as needing a write. */
static void data_bm_touch(uint32_t d) { g.data_bm_dirty[d / FS_BITS_PER_BMBLK] = 1; }

/* Mark an absolute block number used in the data bitmap (used by fsck, which
   writes the whole bitmap itself and so does not track per-block dirtiness). */
static void mark_data(uint32_t blk) {
    if (blk < g.sb.data_start_blk || blk >= g.sb.total_blocks) return;
    bm_set(g.data_bm, blk - g.sb.data_start_blk);
}

/* Allocate a data block: returns an absolute block number, or 0 if full.
   Scans from a rotating cursor (g.next_data) so filling a large file is O(n)
   overall rather than O(n^2). The block is zeroed on disk before being handed
   out so callers never see stale contents (and fresh directory/indirect blocks
   read back as empty). */
static uint32_t data_alloc(void) {
    uint32_t n = g.sb.data_blocks;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t d = g.next_data + k;
        if (d >= n) d -= n;                 /* wrap */
        if (!bm_test(g.data_bm, d)) {
            bm_set(g.data_bm, d);
            data_bm_touch(d);
            g.next_data = (d + 1 < n) ? d + 1 : 0;
            uint32_t blk = g.sb.data_start_blk + d;
            bzero(blk);
            return blk;
        }
    }
    return 0;
}

static void data_free(uint32_t blk) {
    if (blk < g.sb.data_start_blk || blk >= g.sb.total_blocks) return;
    uint32_t d = blk - g.sb.data_start_blk;
    bm_clr(g.data_bm, d);
    data_bm_touch(d);
    if (d < g.next_data) g.next_data = d;   /* prefer reusing freed space */
}

/* (Re)allocate the in-RAM data bitmap + its dirty-flag array from the cached
   superblock. Safe to call again on remount (frees the previous arrays). */
static void data_bm_setup(void) {
    uint32_t dbb = g.sb.data_bitmap_blocks;
    if (g.data_bm)       { kfree(g.data_bm);       g.data_bm       = nullptr; }
    if (g.data_bm_dirty) { kfree(g.data_bm_dirty); g.data_bm_dirty = nullptr; }
    g.data_bm       = (uint8_t *)kmalloc((size_t)dbb * FS_BLOCK_SIZE);
    g.data_bm_dirty = (uint8_t *)kmalloc(dbb);
    if (!g.data_bm || !g.data_bm_dirty)
        panic("fs: out of memory for data bitmap (%u blocks)", dbb);
    kmemset(g.data_bm,       0, (size_t)dbb * FS_BLOCK_SIZE);
    kmemset(g.data_bm_dirty, 0, dbb);
    g.data_bm_blocks = dbb;
    g.next_data      = 0;
}

/* Persist the derived metadata. Always called LAST in a mutating op, after the
   data/inode/directory writes it describes, to preserve the crash ordering.
   Writes only the bitmap blocks actually touched since the last flush (the
   superblock's dirty flag already lives on disk from mount, so it is not
   rewritten per-op; fsck rebuilds the bitmaps after a crash regardless). */
static void flush_meta(void) {
    if (g.inode_bm_dirty) {
        bwrite(g.sb.inode_bitmap_blk, g.inode_bm);
        g.inode_bm_dirty = 0;
    }
    for (uint32_t i = 0; i < g.data_bm_blocks; i++) {
        if (g.data_bm_dirty[i]) {
            bwrite(g.sb.data_bitmap_blk + i, g.data_bm + (size_t)i * FS_BLOCK_SIZE);
            g.data_bm_dirty[i] = 0;
        }
    }
}

/* Force a full rewrite of all metadata bitmaps + the superblock. Used by mkfs
   and unmount where the on-disk copy must be made authoritative. */
static void flush_meta_all(void) {
    g.sb.write_tick = (uint32_t)timer_ticks();
    bwrite(g.sb.inode_bitmap_blk, g.inode_bm);
    for (uint32_t i = 0; i < g.data_bm_blocks; i++)
        bwrite(g.sb.data_bitmap_blk + i, g.data_bm + (size_t)i * FS_BLOCK_SIZE);
    g.inode_bm_dirty = 0;
    kmemset(g.data_bm_dirty, 0, g.data_bm_blocks);
    bwrite(0, &g.sb);
}

/* -------------------------------------------------------------------- inodes */

static void inode_read(uint32_t ino, inode_t *out) {
    uint8_t buf[FS_BLOCK_SIZE];
    uint32_t blk = g.sb.inode_table_blk + ino / FS_INODES_PER_BLOCK;
    bread(blk, buf);
    kmemcpy(out, buf + (ino % FS_INODES_PER_BLOCK) * sizeof(inode_t), sizeof(inode_t));
}

static void inode_write(uint32_t ino, const inode_t *in) {
    uint8_t buf[FS_BLOCK_SIZE];
    uint32_t blk = g.sb.inode_table_blk + ino / FS_INODES_PER_BLOCK;
    bread(blk, buf);
    kmemcpy(buf + (ino % FS_INODES_PER_BLOCK) * sizeof(inode_t), in, sizeof(inode_t));
    bwrite(blk, buf);
}

/* Allocate a fresh inode of `type`, persist it (links=0, caller bumps), and
   return its number. 0 means the inode table is full. */
static uint32_t inode_alloc(uint16_t type) {
    for (uint32_t i = 1; i < g.sb.inode_count; i++) {
        if (!bm_test(g.inode_bm, i)) {
            bm_set(g.inode_bm, i);
            g.inode_bm_dirty = 1;
            inode_t in;
            kmemset(&in, 0, sizeof in);
            in.type  = type;
            in.ctime = in.mtime = (uint32_t)timer_ticks();
            inode_write(i, &in);
            return i;
        }
    }
    return 0;
}

/* Free every block an inode references (data + the indirect block itself),
   zeroing the in-memory pointers. Does not touch the inode bitmap. */
/* Recursively free an indirect tree of height `level` (1=single .. 3=triple)
   rooted at disk block `blk`, including `blk` itself. Buffers come from the
   heap (not the stack) so 3 levels of recursion stay well clear of the 16 KiB
   per-task kernel stack. */
static void free_indirect(uint32_t blk, int level) {
    uint8_t *buf = (uint8_t *)kmalloc(FS_BLOCK_SIZE);
    if (!buf) panic("fs: out of memory freeing indirect block");
    bread(blk, buf);
    uint32_t *ptrs = (uint32_t *)buf;
    for (uint32_t i = 0; i < FS_NPTR_PER_BLOCK; i++) {
        if (!ptrs[i]) continue;
        if (level > 1) free_indirect(ptrs[i], level - 1);
        else           data_free(ptrs[i]);
    }
    kfree(buf);
    data_free(blk);
}

static void blocks_free(inode_t *in) {
    for (uint32_t i = 0; i < FS_NDIRECT; i++) {
        if (in->direct[i]) { data_free(in->direct[i]); in->direct[i] = 0; }
    }
    if (in->indirect)  { free_indirect(in->indirect,  1); in->indirect  = 0; }
    if (in->indirect2) { free_indirect(in->indirect2, 2); in->indirect2 = 0; }
    if (in->indirect3) { free_indirect(in->indirect3, 3); in->indirect3 = 0; }
}

/* Truncate to zero length, keeping the inode allocated as a file. */
static void inode_truncate(uint32_t ino, inode_t *in) {
    blocks_free(in);
    in->size  = 0;
    in->mtime = (uint32_t)timer_ticks();
    inode_write(ino, in);
}

/* Release an inode entirely: free its blocks, clear the record, free the slot. */
static void inode_free(uint32_t ino, inode_t *in) {
    blocks_free(in);
    kmemset(in, 0, sizeof *in);          /* type = FT_NONE */
    inode_write(ino, in);
    bm_clr(g.inode_bm, ino);
    g.inode_bm_dirty = 1;
}

/* Walk an indirect tree of height `level` (1=single .. 3=triple) rooted at the
   pointer *root (which lives in the inode), descending to the data block at
   tree-relative index `idx`. Iterative with a single reused buffer, so stack
   use is O(1) regardless of depth. When `alloc`, missing intermediate and leaf
   blocks are allocated; intermediate-block writes are persisted here, while a
   newly-allocated *root is left for the caller to persist with the inode.
   Returns the data block, or 0 (absent and !alloc, or out of space). */
static uint32_t bmap_indirect(uint32_t *root, int level, uint32_t idx, int alloc) {
    uint8_t buf[FS_BLOCK_SIZE];     /* holds the block that contains `slot` */
    uint32_t *slot = root;          /* address of the pointer to the next node */
    uint32_t parent = 0;            /* disk block holding *slot (0 == inode) */
    int hops = level;               /* indirect blocks still below *slot */

    for (;;) {
        if (!*slot) {
            if (!alloc) return 0;
            uint32_t nb = data_alloc();         /* data_alloc zeroes the block */
            if (!nb) return 0;
            *slot = nb;
            if (parent) bwrite(parent, buf);    /* persist the parent's new ptr */
            /* parent == 0: *slot is an inode field; caller persists the inode. */
        }
        uint32_t blk = *slot;
        if (hops == 0) return blk;              /* reached the data block */

        bread(blk, buf);                         /* descend one level */
        uint32_t *ptrs = (uint32_t *)buf;
        uint32_t per = 1;                        /* leaves covered by each child */
        for (int i = 1; i < hops; i++) per *= FS_NPTR_PER_BLOCK;
        uint32_t ci = idx / per;
        idx %= per;
        parent = blk;
        slot   = &ptrs[ci];
        hops--;
    }
}

/* Map file-block index `fbn` to an absolute disk block. If `alloc`, allocate
   any missing data/indirect blocks and update *in in memory (the caller is
   responsible for persisting the inode). Returns 0 when absent (and !alloc),
   beyond the triple-indirect range, or out of space. */
static uint32_t bmap(inode_t *in, uint32_t fbn, int alloc) {
    if (fbn < FS_NDIRECT) {
        uint32_t b = in->direct[fbn];
        if (!b && alloc) {
            b = data_alloc();
            if (!b) return 0;
            in->direct[fbn] = b;
        }
        return b;
    }
    fbn -= FS_NDIRECT;

    /* The root pointers live in a packed on-disk struct, so operate on an
       aligned local and copy back (the caller persists the inode). */
    uint32_t root, b;
    if (fbn < FS_NINDIRECT) {
        root = in->indirect;
        b = bmap_indirect(&root, 1, fbn, alloc);
        in->indirect = root;
        return b;
    }
    fbn -= FS_NINDIRECT;

    if ((uint64_t)fbn < FS_NDOUBLE) {
        root = in->indirect2;
        b = bmap_indirect(&root, 2, fbn, alloc);
        in->indirect2 = root;
        return b;
    }
    fbn -= (uint32_t)FS_NDOUBLE;

    if ((uint64_t)fbn < FS_NTRIPLE) {
        root = in->indirect3;
        b = bmap_indirect(&root, 3, fbn, alloc);
        in->indirect3 = root;
        return b;
    }

    return 0;                                    /* past triple-indirect reach */
}

/* --------------------------------------------------------------- directories */

static uint32_t round4(uint32_t x) { return (x + 3u) & ~3u; }

/* Count set bits in a byte — we have no libgcc for __builtin_popcount. */
static int popcount8(uint8_t x) {
    int c = 0;
    while (x) { c += x & 1; x >>= 1; }
    return c;
}

/* Look up `name` (nlen bytes) in directory inode `dir`; return child inode or 0. */
static uint32_t dir_lookup(inode_t *dir, const char *name, uint32_t nlen) {
    uint8_t buf[FS_BLOCK_SIZE];
    uint32_t nblk = dir->size / FS_BLOCK_SIZE;
    for (uint32_t fbn = 0; fbn < nblk; fbn++) {
        uint32_t b = bmap(dir, fbn, 0);
        if (!b) continue;
        bread(b, buf);
        uint32_t off = 0;
        while (off < FS_BLOCK_SIZE) {
            dirent_disk_t *de = (dirent_disk_t *)(buf + off);
            uint32_t rlen = de->rec_len;
            if (rlen < FS_DIRENT_HDR || off + rlen > FS_BLOCK_SIZE) break;
            if (de->inode != 0 && (uint32_t)de->name_len == nlen &&
                kmemcmp(buf + off + FS_DIRENT_HDR, name, nlen) == 0)
                return de->inode;
            off += rlen;
        }
    }
    return 0;
}

/* Insert (name -> child_ino) into directory `dir_ino`. Splits a free record or
   an over-long used record; grows the directory by a block if neither fits. */
static int dir_add(uint32_t dir_ino, inode_t *dir, const char *name, uint32_t nlen,
                   uint32_t child_ino, uint8_t type) {
    uint32_t needed = round4(FS_DIRENT_HDR + nlen);
    uint8_t buf[FS_BLOCK_SIZE];
    uint32_t nblk = dir->size / FS_BLOCK_SIZE;

    for (uint32_t fbn = 0; fbn < nblk; fbn++) {
        uint32_t b = bmap(dir, fbn, 0);
        if (!b) continue;
        bread(b, buf);
        uint32_t off = 0;
        while (off < FS_BLOCK_SIZE) {
            dirent_disk_t *de = (dirent_disk_t *)(buf + off);
            uint32_t rlen = de->rec_len;
            if (rlen < FS_DIRENT_HDR || off + rlen > FS_BLOCK_SIZE) break;

            if (de->inode == 0 && rlen >= needed) {
                de->inode    = child_ino;
                de->name_len = (uint8_t)nlen;
                de->type     = type;
                kmemcpy(buf + off + FS_DIRENT_HDR, name, nlen);
                bwrite(b, buf);
                return FS_OK;
            }
            if (de->inode != 0) {
                uint32_t used = round4(FS_DIRENT_HDR + de->name_len);
                if (rlen - used >= needed) {
                    de->rec_len = (uint16_t)used;
                    dirent_disk_t *nd = (dirent_disk_t *)(buf + off + used);
                    nd->inode    = child_ino;
                    nd->rec_len  = (uint16_t)(rlen - used);
                    nd->name_len = (uint8_t)nlen;
                    nd->type     = type;
                    kmemcpy(buf + off + used + FS_DIRENT_HDR, name, nlen);
                    bwrite(b, buf);
                    return FS_OK;
                }
            }
            off += rlen;
        }
    }

    /* No slack anywhere: append a fresh directory block. */
    uint32_t b = bmap(dir, nblk, 1);
    if (!b) return FS_ENOSPC;
    kmemset(buf, 0, sizeof buf);
    dirent_disk_t *de = (dirent_disk_t *)buf;
    de->inode    = child_ino;
    de->rec_len  = (uint16_t)FS_BLOCK_SIZE;
    de->name_len = (uint8_t)nlen;
    de->type     = type;
    kmemcpy(buf + FS_DIRENT_HDR, name, nlen);
    bwrite(b, buf);
    dir->size += FS_BLOCK_SIZE;
    inode_write(dir_ino, dir);          /* persist new block pointer + size */
    return FS_OK;
}

/* Remove `name` from directory `dir_ino`. Frees the slot by folding it into the
   previous record (or zeroing its inode if it is first). Reports the removed
   inode number via *removed_ino. */
static int dir_remove(uint32_t dir_ino, inode_t *dir, const char *name, uint32_t nlen,
                      uint32_t *removed_ino) {
    (void)dir_ino;
    uint8_t buf[FS_BLOCK_SIZE];
    uint32_t nblk = dir->size / FS_BLOCK_SIZE;
    for (uint32_t fbn = 0; fbn < nblk; fbn++) {
        uint32_t b = bmap(dir, fbn, 0);
        if (!b) continue;
        bread(b, buf);
        uint32_t off = 0, prev = (uint32_t)-1;
        while (off < FS_BLOCK_SIZE) {
            dirent_disk_t *de = (dirent_disk_t *)(buf + off);
            uint32_t rlen = de->rec_len;
            if (rlen < FS_DIRENT_HDR || off + rlen > FS_BLOCK_SIZE) break;
            if (de->inode != 0 && (uint32_t)de->name_len == nlen &&
                kmemcmp(buf + off + FS_DIRENT_HDR, name, nlen) == 0) {
                if (removed_ino) *removed_ino = de->inode;
                if (prev != (uint32_t)-1) {
                    dirent_disk_t *pd = (dirent_disk_t *)(buf + prev);
                    pd->rec_len = (uint16_t)(pd->rec_len + rlen);
                } else {
                    de->inode = 0;          /* first record: just free it */
                }
                bwrite(b, buf);
                return FS_OK;
            }
            prev = off;
            off += rlen;
        }
    }
    return FS_ENOENT;
}

/* True if a directory holds only "." and "..". */
static int dir_empty(inode_t *dir) {
    uint8_t buf[FS_BLOCK_SIZE];
    uint32_t nblk = dir->size / FS_BLOCK_SIZE;
    for (uint32_t fbn = 0; fbn < nblk; fbn++) {
        uint32_t b = bmap(dir, fbn, 0);
        if (!b) continue;
        bread(b, buf);
        uint32_t off = 0;
        while (off < FS_BLOCK_SIZE) {
            dirent_disk_t *de = (dirent_disk_t *)(buf + off);
            uint32_t rlen = de->rec_len;
            if (rlen < FS_DIRENT_HDR || off + rlen > FS_BLOCK_SIZE) break;
            if (de->inode != 0) {
                const char *nm = (const char *)(buf + off + FS_DIRENT_HDR);
                int dot    = (de->name_len == 1 && nm[0] == '.');
                int dotdot = (de->name_len == 2 && nm[0] == '.' && nm[1] == '.');
                if (!dot && !dotdot) return 0;
            }
            off += rlen;
        }
    }
    return 1;
}

/* --------------------------------------------------------------------- paths */

#define FS_SYMLINK_MAX  8       /* max symlinks followed in one resolution */

/* Walk `p` (relative, components separated by '/') starting at directory inode
   `ino`, following symlinks. A symlink in a non-final position is always
   followed; a final symlink is followed only if `follow_final`. `.`/`..` resolve
   naturally because every directory carries them as real entries. Returns the
   inode number, or 0 on a missing/!dir component or a symlink loop. */
static uint32_t resolve_from(uint32_t ino, const char *p, int follow_final, int depth) {
    if (depth > FS_SYMLINK_MAX) return 0;
    while (*p == '/') p++;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t nlen = (uint32_t)(p - start);
        while (*p == '/') p++;
        int is_final = (*p == 0);
        if (nlen == 0) continue;

        inode_t dir;
        inode_read(ino, &dir);
        if (dir.type != FT_DIR) return 0;
        uint32_t child = dir_lookup(&dir, start, nlen);
        if (!child) return 0;

        inode_t ci;
        inode_read(child, &ci);
        if (ci.type == FT_SYMLINK && (!is_final || follow_final)) {
            char target[256];
            uint32_t tlen = ci.size < sizeof target - 1 ? (uint32_t)ci.size : sizeof target - 1;
            uint32_t b0 = bmap(&ci, 0, 0);
            if (b0) { uint8_t tb[FS_BLOCK_SIZE]; bread(b0, tb); kmemcpy(target, tb, tlen); }
            else tlen = 0;
            target[tlen] = '\0';
            uint32_t base = (target[0] == '/') ? g.sb.root_inode : ino;  /* containing dir */
            uint32_t tino = resolve_from(base, target, 1, depth + 1);
            if (!tino) return 0;
            ino = tino;                          /* continue walk from the target */
        } else {
            ino = child;
        }
    }
    return ino;
}

/* Resolve an absolute path to an inode number (follows a final symlink), or 0. */
static uint32_t path_resolve(const char *path) {
    if (!path || path[0] != '/') return 0;
    return resolve_from(g.sb.root_inode, path + 1, 1, 0);
}

/* Like path_resolve but does not follow a trailing symlink (for lstat/readlink). */
static uint32_t path_lresolve(const char *path) {
    if (!path || path[0] != '/') return 0;
    return resolve_from(g.sb.root_inode, path + 1, 0, 0);
}

/* Split a path into (parent inode, final component). On success *parent is the
   parent directory's inode and name/nlen point at the last component. */
static int path_parent(const char *path, uint32_t *parent,
                        const char **name, uint32_t *nlen) {
    if (!path || path[0] != '/') return FS_EINVAL;

    const char *end = path + kstrlen(path);
    while (end > path + 1 && end[-1] == '/') end--;          /* strip trailing / */
    const char *last = end;
    while (last > path && last[-1] != '/') last--;

    uint32_t ln = (uint32_t)(end - last);
    if (ln == 0)            return FS_EINVAL;                /* path was "/" */
    if (ln > FS_NAME_MAX)   return FS_ENAMETOOLONG;

    if (last == path + 1) {
        *parent = g.sb.root_inode;
    } else {
        char pbuf[256];
        uint32_t plen = (uint32_t)(last - path);
        while (plen > 1 && path[plen - 1] == '/') plen--;
        if (plen >= sizeof pbuf) return FS_ENAMETOOLONG;
        kmemcpy(pbuf, path, plen);
        pbuf[plen] = 0;
        uint32_t pino = path_resolve(pbuf);
        if (!pino) return FS_ENOENT;
        *parent = pino;
    }
    *name = last;
    *nlen = ln;
    return FS_OK;
}

/* ----------------------------------------------------------------------- mkfs */

static void mkfs(void) {
    uint32_t total = (uint32_t)(g.dev->num_blocks / g.spb);
    if (total < 8) panic("fs: device too small (%u FS blocks)", total);

    uint32_t inode_count = total / 4;
    if (inode_count < 16)    inode_count = 16;
    if (inode_count > 32768) inode_count = 32768;
    inode_count = (inode_count + 31u) & ~31u;               /* multiple of 32 */

    uint32_t IT = inode_count / FS_INODES_PER_BLOCK;

    /* Layout: sb | inode bitmap (1 blk) | data bitmap (DB blks) | inode table
       (IT blks) | data area. The data bitmap must cover the data blocks, whose
       count depends on DB in turn; solving the recurrence, DB = ceil(after /
       bits_per_block) where `after` is everything past the inode table covers
       both the bitmap and the data, which guarantees coverage. */
    uint32_t inode_bm_blocks = 1;               /* <=32768 inodes -> 1 block */
    if (1u + inode_bm_blocks + IT >= total) panic("fs: no room for data area");
    uint32_t after = total - 1u - inode_bm_blocks - IT;   /* data bitmap + data */
    uint32_t DB = (after + FS_BITS_PER_BMBLK - 1) / FS_BITS_PER_BMBLK;
    if (DB == 0) DB = 1;
    if (DB >= after) panic("fs: no room for data area");

    uint32_t data_bitmap_blk = 1u + inode_bm_blocks;      /* right after inode bm */
    uint32_t inode_table_blk = data_bitmap_blk + DB;
    uint32_t data_start      = inode_table_blk + IT;

    kmemset(&g.sb, 0, sizeof g.sb);
    g.sb.magic               = FS_MAGIC;
    g.sb.version             = FS_VERSION;
    g.sb.block_size          = FS_BLOCK_SIZE;
    g.sb.total_blocks        = total;
    g.sb.inode_count         = inode_count;
    g.sb.inode_bitmap_blk    = 1;
    g.sb.inode_bitmap_blocks = inode_bm_blocks;
    g.sb.data_bitmap_blk     = data_bitmap_blk;
    g.sb.data_bitmap_blocks  = DB;
    g.sb.inode_table_blk     = inode_table_blk;
    g.sb.inode_table_blocks  = IT;
    g.sb.data_start_blk      = data_start;
    g.sb.data_blocks         = total - data_start;
    g.sb.root_inode          = 1;
    g.sb.dirty               = 0;

    /* Now that the layout (and DB) is known, size the in-RAM data bitmap. */
    data_bm_setup();

    /* Empty bitmaps; inode 0 (reserved) and inode 1 (root) are in use. */
    kmemset(g.inode_bm, 0, sizeof g.inode_bm);
    bm_set(g.inode_bm, 0);
    bm_set(g.inode_bm, 1);
    for (uint32_t i = 0; i < IT; i++) bzero(g.sb.inode_table_blk + i);

    /* Root directory: one data block holding "." and ".." (both -> root). */
    inode_t root;
    kmemset(&root, 0, sizeof root);
    root.type  = FT_DIR;
    root.links = 2;
    root.ctime = root.mtime = (uint32_t)timer_ticks();
    uint32_t b = data_alloc();
    if (!b) panic("fs: mkfs cannot allocate root block");
    root.direct[0] = b;
    root.size      = FS_BLOCK_SIZE;

    uint8_t buf[FS_BLOCK_SIZE];
    kmemset(buf, 0, sizeof buf);
    uint32_t l1 = round4(FS_DIRENT_HDR + 1);
    dirent_disk_t *d1 = (dirent_disk_t *)buf;
    d1->inode = 1; d1->rec_len = (uint16_t)l1; d1->name_len = 1; d1->type = FT_DIR;
    buf[FS_DIRENT_HDR] = '.';
    dirent_disk_t *d2 = (dirent_disk_t *)(buf + l1);
    d2->inode = 1; d2->rec_len = (uint16_t)(FS_BLOCK_SIZE - l1);
    d2->name_len = 2; d2->type = FT_DIR;
    buf[l1 + FS_DIRENT_HDR] = '.'; buf[l1 + FS_DIRENT_HDR + 1] = '.';
    bwrite(b, buf);
    inode_write(1, &root);

    flush_meta_all();          /* write both bitmaps + the superblock in full */
}

/* ----------------------------------------------------------------------- fsck */

/* Mark every block of an indirect tree of height `level` (including the tree's
   own pointer blocks) used in the data bitmap. Mirror of free_indirect; uses a
   heap buffer so triple-indirect recursion stays off the kernel stack. */
static void mark_indirect(uint32_t blk, int level) {
    mark_data(blk);
    uint8_t *buf = (uint8_t *)kmalloc(FS_BLOCK_SIZE);
    if (!buf) panic("fsck: out of memory marking indirect block");
    bread(blk, buf);
    uint32_t *p = (uint32_t *)buf;
    for (uint32_t i = 0; i < FS_NPTR_PER_BLOCK; i++) {
        if (!p[i]) continue;
        if (level > 1) mark_indirect(p[i], level - 1);
        else           mark_data(p[i]);
    }
    kfree(buf);
}

/*
 * Repair an unclean volume. The inode table is authoritative, so we:
 *   pass 1 — scan every directory: count references per inode, drop entries
 *            that point at free/out-of-range inodes;
 *   pass 2 — fix each inode's link count to the counted value, and free any
 *            in-use non-root inode that nothing references (an orphan);
 *   pass 3 — rebuild both bitmaps from the surviving inodes' block pointers.
 * Every repair is logged. Covers the four cases the roadmap calls out:
 * bitmap/inode mismatch, dangling dir entries, link-count drift, orphans.
 */
static void fsck(void) {
    uint32_t n = g.sb.inode_count;
    uint16_t *linkcnt = (uint16_t *)kmalloc(n * sizeof(uint16_t));
    if (!linkcnt) panic("fsck: out of memory");
    kmemset(linkcnt, 0, n * sizeof(uint16_t));
    int repairs = 0;

    /* Pass 1: tally directory references; drop dangling entries. */
    for (uint32_t ino = 1; ino < n; ino++) {
        inode_t in;
        inode_read(ino, &in);
        if (in.type != FT_DIR) continue;
        uint32_t nblk = in.size / FS_BLOCK_SIZE;
        uint8_t buf[FS_BLOCK_SIZE];
        for (uint32_t fbn = 0; fbn < nblk; fbn++) {
            uint32_t b = bmap(&in, fbn, 0);
            if (!b) continue;
            bread(b, buf);
            int blk_dirty = 0;
            uint32_t off = 0;
            while (off < FS_BLOCK_SIZE) {
                dirent_disk_t *de = (dirent_disk_t *)(buf + off);
                uint32_t rlen = de->rec_len;
                if (rlen < FS_DIRENT_HDR || off + rlen > FS_BLOCK_SIZE) break;
                if (de->inode != 0) {
                    uint32_t t = de->inode;
                    int ok = 0;
                    if (t < n) {
                        inode_t ti;
                        inode_read(t, &ti);
                        ok = (ti.type != FT_NONE);
                    }
                    if (ok) {
                        linkcnt[t]++;
                    } else {
                        kprintf("[fsck] dir %u: dropping entry -> dead inode %u\n", ino, t);
                        de->inode = 0;
                        blk_dirty = 1;
                        repairs++;
                    }
                }
                off += rlen;
            }
            if (blk_dirty) bwrite(b, buf);
        }
    }

    /* Pass 2: correct link counts; free orphans. */
    for (uint32_t ino = 1; ino < n; ino++) {
        inode_t in;
        inode_read(ino, &in);
        if (in.type == FT_NONE) continue;

        if (ino != g.sb.root_inode && linkcnt[ino] == 0) {
            kprintf("[fsck] inode %u: orphan (0 refs) -> freeing\n", ino);
            blocks_free(&in);
            kmemset(&in, 0, sizeof in);
            inode_write(ino, &in);
            repairs++;
            continue;
        }
        if (in.links != linkcnt[ino]) {
            kprintf("[fsck] inode %u: link count %u -> %u\n",
                    ino, in.links, linkcnt[ino]);
            in.links = linkcnt[ino];
            inode_write(ino, &in);
            repairs++;
        }
    }

    /* Pass 3: rebuild both bitmaps from the inodes that survived, diffing
       against what was on disk so a corrupted/torn bitmap is reported. */
    uint32_t db_bytes = g.data_bm_blocks * FS_BLOCK_SIZE;
    uint8_t *old_ib = (uint8_t *)kmalloc(FS_BLOCK_SIZE);
    uint8_t *old_db = (uint8_t *)kmalloc(db_bytes);
    if (!old_ib || !old_db) panic("fsck: out of memory");
    kmemcpy(old_ib, g.inode_bm, FS_BLOCK_SIZE);
    kmemcpy(old_db, g.data_bm,  db_bytes);

    kmemset(g.inode_bm, 0, sizeof g.inode_bm);
    kmemset(g.data_bm,  0, db_bytes);
    bm_set(g.inode_bm, 0);
    for (uint32_t ino = 1; ino < n; ino++) {
        inode_t in;
        inode_read(ino, &in);
        if (in.type == FT_NONE) continue;
        bm_set(g.inode_bm, ino);
        for (uint32_t i = 0; i < FS_NDIRECT; i++)
            if (in.direct[i]) mark_data(in.direct[i]);
        if (in.indirect)  mark_indirect(in.indirect,  1);
        if (in.indirect2) mark_indirect(in.indirect2, 2);
        if (in.indirect3) mark_indirect(in.indirect3, 3);
    }

    int ib_fixed = 0, db_fixed = 0;
    for (uint32_t i = 0; i < FS_BLOCK_SIZE; i++)
        ib_fixed += popcount8((uint8_t)(old_ib[i] ^ g.inode_bm[i]));
    for (uint32_t i = 0; i < db_bytes; i++)
        db_fixed += popcount8((uint8_t)(old_db[i] ^ g.data_bm[i]));
    if (ib_fixed) { kprintf("[fsck] inode bitmap: corrected %d bit(s)\n", ib_fixed); repairs += ib_fixed; }
    if (db_fixed) { kprintf("[fsck] data bitmap: corrected %d bit(s)\n", db_fixed); repairs += db_fixed; }
    kfree(old_ib);
    kfree(old_db);

    flush_meta_all();          /* rewrite both bitmaps (all blocks) + superblock */
    kfree(linkcnt);
    kprintf("[fsck] complete: %d repair(s)\n", repairs);
}

/* ------------------------------------------------------------- mount/unmount */

/* Load both cached bitmaps from disk into RAM (all data-bitmap blocks). */
static void bitmaps_load(void) {
    bread(g.sb.inode_bitmap_blk, g.inode_bm);
    for (uint32_t i = 0; i < g.data_bm_blocks; i++)
        bread(g.sb.data_bitmap_blk + i, g.data_bm + (size_t)i * FS_BLOCK_SIZE);
    g.inode_bm_dirty = 0;
    kmemset(g.data_bm_dirty, 0, g.data_bm_blocks);
}

int fs_mount(block_device_t *dev) {
    if (!dev) return FS_EINVAL;
    if (dev->block_size == 0 || FS_BLOCK_SIZE % dev->block_size != 0)
        panic("fs: unsupported device block size %u", dev->block_size);

    g.dev = dev;
    g.spb = FS_BLOCK_SIZE / dev->block_size;
    for (int i = 0; i < FS_MAX_OPEN; i++) g_fds[i].used = 0;

    bread(0, &g.sb);
    if (g.sb.magic != FS_MAGIC || g.sb.version != FS_VERSION ||
        g.sb.block_size != FS_BLOCK_SIZE) {
        kprintf("[fs] %s: no VIBEFS volume (magic=%x) -> formatting\n",
                dev->dev.name, g.sb.magic);
        mkfs();
        bread(0, &g.sb);
    }

    data_bm_setup();        /* size the in-RAM data bitmap from the superblock */
    bitmaps_load();

    if (g.sb.dirty) {
        kprintf("[fs] %s: unclean unmount -> running fsck\n", dev->dev.name);
        fsck();
        bitmaps_load();
    }

    g.sb.dirty      = 1;
    g.sb.mount_tick = (uint32_t)timer_ticks();
    bwrite(0, &g.sb);
    g.mounted = 1;

    kprintf("[fs] mounted %s: %u blocks, %u inodes, data@%u (%u blocks), "
            "data-bitmap %u blk(s), max file %lu MiB\n",
            dev->dev.name, g.sb.total_blocks, g.sb.inode_count,
            g.sb.data_start_blk, g.sb.data_blocks, g.sb.data_bitmap_blocks,
            (unsigned long)(((uint64_t)g.sb.data_blocks * FS_BLOCK_SIZE) >> 20));
    return FS_OK;
}

int fs_unmount(void) {
    if (!g.mounted) return FS_EINVAL;
    flush_meta_all();           /* both bitmaps in full + superblock */
    g.sb.dirty      = 0;
    g.sb.write_tick = (uint32_t)timer_ticks();
    bwrite(0, &g.sb);
    g.mounted = 0;
    for (int i = 0; i < FS_MAX_OPEN; i++) g_fds[i].used = 0;
    return FS_OK;
}

/* ------------------------------------------------------------- create/mkdir */

int fs_create(const char *path) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t parent, nlen;
    const char *name;
    int r = path_parent(path, &parent, &name, &nlen);
    if (r != FS_OK) return r;

    inode_t pdir;
    inode_read(parent, &pdir);
    if (pdir.type != FT_DIR) return FS_ENOTDIR;
    if (dir_lookup(&pdir, name, nlen)) return FS_EEXIST;

    uint32_t ino = inode_alloc(FT_REG);
    if (!ino) return FS_ENOSPC;
    inode_t in;
    inode_read(ino, &in);
    in.links = 1;
    inode_write(ino, &in);                          /* inode before dirent */

    r = dir_add(parent, &pdir, name, nlen, ino, FT_REG);
    if (r != FS_OK) { inode_free(ino, &in); flush_meta(); return r; }

    flush_meta();
    return FS_OK;
}

int fs_mkdir(const char *path) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t parent, nlen;
    const char *name;
    int r = path_parent(path, &parent, &name, &nlen);
    if (r != FS_OK) return r;

    inode_t pdir;
    inode_read(parent, &pdir);
    if (pdir.type != FT_DIR) return FS_ENOTDIR;
    if (dir_lookup(&pdir, name, nlen)) return FS_EEXIST;

    uint32_t ino = inode_alloc(FT_DIR);
    if (!ino) return FS_ENOSPC;
    inode_t in;
    inode_read(ino, &in);

    uint32_t b = bmap(&in, 0, 1);
    if (!b) { inode_free(ino, &in); flush_meta(); return FS_ENOSPC; }
    in.size  = FS_BLOCK_SIZE;
    in.links = 2;                                   /* "." + entry in parent */

    uint8_t buf[FS_BLOCK_SIZE];
    kmemset(buf, 0, sizeof buf);
    uint32_t l1 = round4(FS_DIRENT_HDR + 1);
    dirent_disk_t *d1 = (dirent_disk_t *)buf;
    d1->inode = ino; d1->rec_len = (uint16_t)l1; d1->name_len = 1; d1->type = FT_DIR;
    buf[FS_DIRENT_HDR] = '.';
    dirent_disk_t *d2 = (dirent_disk_t *)(buf + l1);
    d2->inode = parent; d2->rec_len = (uint16_t)(FS_BLOCK_SIZE - l1);
    d2->name_len = 2; d2->type = FT_DIR;
    buf[l1 + FS_DIRENT_HDR] = '.'; buf[l1 + FS_DIRENT_HDR + 1] = '.';
    bwrite(b, buf);                                 /* data block */
    inode_write(ino, &in);                          /* inode */

    r = dir_add(parent, &pdir, name, nlen, ino, FT_DIR);   /* dirent */
    if (r != FS_OK) { inode_free(ino, &in); flush_meta(); return r; }

    pdir.links += 1;                                /* child's ".." -> parent */
    inode_write(parent, &pdir);

    flush_meta();
    return FS_OK;
}

/* --------------------------------------------------------------------- unlink */

int fs_unlink(const char *path) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t parent, nlen;
    const char *name;
    int r = path_parent(path, &parent, &name, &nlen);
    if (r != FS_OK) return r;

    inode_t pdir;
    inode_read(parent, &pdir);
    if (pdir.type != FT_DIR) return FS_ENOTDIR;

    uint32_t ino = dir_lookup(&pdir, name, nlen);
    if (!ino) return FS_ENOENT;

    inode_t in;
    inode_read(ino, &in);
    if (in.type == FT_DIR && !dir_empty(&in)) return FS_ENOTEMPTY;

    uint32_t removed = 0;
    r = dir_remove(parent, &pdir, name, nlen, &removed);    /* visibility first */
    if (r != FS_OK) return r;

    if (in.type == FT_DIR) {
        pdir.links -= 1;                            /* lost the child's ".." */
        inode_write(parent, &pdir);
        inode_free(ino, &in);                       /* frees data + slot */
    } else {
        in.links -= 1;
        if (in.links == 0) inode_free(ino, &in);
        else               inode_write(ino, &in);
    }

    flush_meta();
    return FS_OK;
}

/* ---------------------------------------------------------------- open/close */

int fs_open(const char *path, int flags) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t ino = path_resolve(path);
    if (!ino) {
        if (!(flags & FS_O_CREATE)) return FS_ENOENT;
        int r = fs_create(path);
        if (r != FS_OK) return r;
        ino = path_resolve(path);
        if (!ino) return FS_EIO;
    }

    inode_t in;
    inode_read(ino, &in);
    if (in.type == FT_DIR) return FS_EISDIR;

    if (flags & FS_O_TRUNC) {
        inode_truncate(ino, &in);
        flush_meta();
    }

    int fd = -1;
    for (int i = 0; i < FS_MAX_OPEN; i++) if (!g_fds[i].used) { fd = i; break; }
    if (fd < 0) return FS_EMFILE;
    g_fds[fd].used = 1;
    g_fds[fd].ino  = ino;
    g_fds[fd].off  = 0;
    return fd;
}

int fs_close(int fd) {
    if (fd < 0 || fd >= FS_MAX_OPEN || !g_fds[fd].used) return FS_EBADF;
    g_fds[fd].used = 0;
    return FS_OK;
}

/* ----------------------------------------------------------------- read/write */

int fs_read(int fd, void *buf, uint32_t n) {
    if (fd < 0 || fd >= FS_MAX_OPEN || !g_fds[fd].used) return FS_EBADF;
    inode_t in;
    inode_read(g_fds[fd].ino, &in);

    uint64_t off = g_fds[fd].off;
    if (off >= in.size) return 0;
    if ((uint64_t)n > in.size - off) n = (uint32_t)(in.size - off);

    uint8_t blk[FS_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < n) {
        uint32_t fbn   = (uint32_t)(off / FS_BLOCK_SIZE);
        uint32_t bo    = (uint32_t)(off % FS_BLOCK_SIZE);
        uint32_t chunk = FS_BLOCK_SIZE - bo;
        if (chunk > n - done) chunk = n - done;
        uint32_t b = bmap(&in, fbn, 0);
        if (b) { bread(b, blk); kmemcpy((uint8_t *)buf + done, blk + bo, chunk); }
        else   { kmemset((uint8_t *)buf + done, 0, chunk); }    /* sparse hole */
        done += chunk;
        off  += chunk;
    }
    g_fds[fd].off = off;
    return (int)done;
}

int fs_write(int fd, const void *buf, uint32_t n) {
    if (fd < 0 || fd >= FS_MAX_OPEN || !g_fds[fd].used) return FS_EBADF;
    inode_t in;
    inode_read(g_fds[fd].ino, &in);

    uint64_t off = g_fds[fd].off;
    if (off + n > FS_MAXFILEBLKS * (uint64_t)FS_BLOCK_SIZE)
        return FS_EFBIG;

    uint8_t blk[FS_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < n) {
        uint32_t fbn   = (uint32_t)(off / FS_BLOCK_SIZE);
        uint32_t bo    = (uint32_t)(off % FS_BLOCK_SIZE);
        uint32_t chunk = FS_BLOCK_SIZE - bo;
        if (chunk > n - done) chunk = n - done;
        uint32_t b = bmap(&in, fbn, 1);
        if (!b) break;                              /* out of space; keep what we wrote */
        if (chunk < FS_BLOCK_SIZE) bread(b, blk);   /* RMW partial block */
        kmemcpy(blk + bo, (const uint8_t *)buf + done, chunk);
        bwrite(b, blk);                             /* data first */
        done += chunk;
        off  += chunk;
    }

    if (off > in.size) in.size = off;
    in.mtime = (uint32_t)timer_ticks();
    inode_write(g_fds[fd].ino, &in);                /* inode (size+ptrs) next */
    g_fds[fd].off = off;
    flush_meta();                                   /* touched bitmap blocks last */
    return (int)done;
}

/* Set the absolute file offset. Seeking past EOF is allowed; a later write
   creates a sparse file (intervening blocks read back as zeros). */
int fs_seek(int fd, uint64_t off) {
    if (fd < 0 || fd >= FS_MAX_OPEN || !g_fds[fd].used) return FS_EBADF;
    if (off > FS_MAXFILEBLKS * (uint64_t)FS_BLOCK_SIZE) return FS_EFBIG;
    g_fds[fd].off = off;
    return FS_OK;
}

/* ---- §4 rung 2: inode-level primitives for the per-process fd layer (fs.h) ---- */

int fs_resolve(const char *path) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t ino = path_resolve(path);
    if (!ino) return FS_ENOENT;
    return (int)ino;
}

int fs_lresolve(const char *path) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t ino = path_lresolve(path);
    if (!ino) return FS_ENOENT;
    return (int)ino;
}

/* Create a symbolic link at `linkpath` whose target is `target`. The target
   string is stored as the link inode's file data (crash-safe order: data ->
   inode -> dirent), so a symlink is just a small file of type FT_SYMLINK. */
int fs_symlink(const char *target, const char *linkpath) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t tlen = (uint32_t)kstrlen(target);
    if (tlen == 0 || tlen >= FS_BLOCK_SIZE) return FS_EINVAL;

    uint32_t parent, nlen;
    const char *name;
    int r = path_parent(linkpath, &parent, &name, &nlen);
    if (r != FS_OK) return r;

    inode_t pdir;
    inode_read(parent, &pdir);
    if (pdir.type != FT_DIR) return FS_ENOTDIR;
    if (dir_lookup(&pdir, name, nlen)) return FS_EEXIST;

    uint32_t ino = inode_alloc(FT_SYMLINK);
    if (!ino) return FS_ENOSPC;
    inode_t in;
    inode_read(ino, &in);
    uint32_t b = bmap(&in, 0, 1);                    /* one data block for the target */
    if (!b) { inode_free(ino, &in); flush_meta(); return FS_ENOSPC; }
    uint8_t buf[FS_BLOCK_SIZE];
    kmemset(buf, 0, sizeof buf);
    kmemcpy(buf, target, tlen);
    bwrite(b, buf);
    in.size = tlen;
    in.links = 1;
    inode_write(ino, &in);                           /* inode before dirent */

    r = dir_add(parent, &pdir, name, nlen, ino, FT_SYMLINK);
    if (r != FS_OK) { inode_free(ino, &in); flush_meta(); return r; }
    flush_meta();
    return FS_OK;
}

int fs_readlink(uint32_t ino, char *buf, uint32_t bufsz) {
    if (!g.mounted) return FS_EINVAL;
    inode_t in;
    inode_read(ino, &in);
    if (in.type != FT_SYMLINK) return FS_EINVAL;
    uint32_t n = in.size < bufsz ? (uint32_t)in.size : bufsz;
    uint32_t b0 = bmap(&in, 0, 0);
    if (b0) { uint8_t tb[FS_BLOCK_SIZE]; bread(b0, tb); kmemcpy(buf, tb, n); }
    else n = 0;
    return (int)n;
}

int fs_istat(uint32_t ino, fs_stat_t *out) {
    if (!g.mounted) return FS_EINVAL;
    inode_t in;
    inode_read(ino, &in);
    if (in.type == FT_NONE) return FS_ENOENT;
    out->type  = in.type;
    out->links = in.links;
    out->size  = in.size;
    out->ctime = in.ctime;
    out->mtime = in.mtime;
    return FS_OK;
}

int fs_truncate_ino(uint32_t ino) {
    if (!g.mounted) return FS_EINVAL;
    inode_t in;
    inode_read(ino, &in);
    if (in.type == FT_DIR) return FS_EISDIR;
    inode_truncate(ino, &in);
    flush_meta();
    return FS_OK;
}

/* Read up to n bytes from inode `ino` at absolute offset `off`. Returns bytes
   read (0 at EOF). Mirrors fs_read but without the global-fd offset. */
int fs_pread(uint32_t ino, uint64_t off, void *buf, uint32_t n) {
    if (!g.mounted) return FS_EINVAL;
    inode_t in;
    inode_read(ino, &in);
    if (off >= in.size) return 0;
    uint64_t avail = in.size - off;
    if (n > avail) n = (uint32_t)avail;
    uint8_t blk[FS_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < n) {
        uint32_t fbn  = (uint32_t)((off + done) / FS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)((off + done) % FS_BLOCK_SIZE);
        uint32_t chunk = FS_BLOCK_SIZE - boff;
        if (chunk > n - done) chunk = n - done;
        uint32_t b = bmap(&in, fbn, 0);
        if (b) { bread(b, blk); kmemcpy((uint8_t *)buf + done, blk + boff, chunk); }
        else   { kmemset((uint8_t *)buf + done, 0, chunk); }   /* sparse hole */
        done += chunk;
    }
    return (int)done;
}

/* Write n bytes to inode `ino` at absolute offset `off`, growing the file as
   needed. Returns bytes written. Mirrors fs_write. */
int fs_pwrite(uint32_t ino, uint64_t off, const void *buf, uint32_t n) {
    if (!g.mounted) return FS_EINVAL;
    inode_t in;
    inode_read(ino, &in);
    if (in.type == FT_DIR) return FS_EISDIR;
    if (off + n > FS_MAXFILEBLKS * (uint64_t)FS_BLOCK_SIZE) return FS_EFBIG;
    uint8_t blk[FS_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < n) {
        uint32_t fbn  = (uint32_t)((off + done) / FS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)((off + done) % FS_BLOCK_SIZE);
        uint32_t b = bmap(&in, fbn, 1);
        if (!b) { if (done == 0) return FS_ENOSPC; break; }
        uint32_t chunk = FS_BLOCK_SIZE - boff;
        if (chunk > n - done) chunk = n - done;
        if (chunk < FS_BLOCK_SIZE) bread(b, blk);   /* RMW partial block */
        kmemcpy(blk + boff, (const uint8_t *)buf + done, chunk);
        bwrite(b, blk);
        done += chunk;
    }
    if (off + done > in.size) in.size = off + done;
    in.mtime = (uint32_t)timer_ticks();
    inode_write(ino, &in);
    if (done) flush_meta();
    return (int)done;
}

/* Return one directory entry at byte cursor *pos (advancing it past the
   consumed record). Skips free slots. Records never span a block. */
int fs_dirent_at(uint32_t ino, uint64_t *pos, fs_dirent_t *out) {
    if (!g.mounted) return FS_EINVAL;
    inode_t in;
    inode_read(ino, &in);
    if (in.type != FT_DIR) return FS_ENOTDIR;
    uint64_t size = in.size;
    uint8_t buf[FS_BLOCK_SIZE];
    while (*pos < size) {
        uint32_t fbn  = (uint32_t)(*pos / FS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)(*pos % FS_BLOCK_SIZE);
        uint32_t b = bmap(&in, fbn, 0);
        if (!b) { *pos = (uint64_t)(fbn + 1) * FS_BLOCK_SIZE; continue; }
        bread(b, buf);
        dirent_disk_t *de = (dirent_disk_t *)(buf + boff);
        uint32_t rlen = de->rec_len;
        if (rlen < FS_DIRENT_HDR || boff + rlen > FS_BLOCK_SIZE) {
            *pos = (uint64_t)(fbn + 1) * FS_BLOCK_SIZE;   /* skip to next block */
            continue;
        }
        uint64_t next = *pos + rlen;
        if (de->inode != 0) {
            uint32_t nl = de->name_len;
            if (nl > FS_NAME_MAX) nl = FS_NAME_MAX;
            kmemcpy(out->name, buf + boff + FS_DIRENT_HDR, nl);
            out->name[nl] = 0;
            out->inode = de->inode;
            out->type  = de->type;
            *pos = next;
            return 1;
        }
        *pos = next;
    }
    return 0;
}

/* -------------------------------------------------------------------- readdir */

int fs_readdir(const char *path, fs_dirent_t *out, int max) {
    if (!g.mounted) return FS_EINVAL;
    uint32_t ino = path_resolve(path);
    if (!ino) return FS_ENOENT;
    inode_t in;
    inode_read(ino, &in);
    if (in.type != FT_DIR) return FS_ENOTDIR;

    uint8_t buf[FS_BLOCK_SIZE];
    int cnt = 0;
    uint32_t nblk = in.size / FS_BLOCK_SIZE;
    for (uint32_t fbn = 0; fbn < nblk && cnt < max; fbn++) {
        uint32_t b = bmap(&in, fbn, 0);
        if (!b) continue;
        bread(b, buf);
        uint32_t off = 0;
        while (off < FS_BLOCK_SIZE && cnt < max) {
            dirent_disk_t *de = (dirent_disk_t *)(buf + off);
            uint32_t rlen = de->rec_len;
            if (rlen < FS_DIRENT_HDR || off + rlen > FS_BLOCK_SIZE) break;
            if (de->inode != 0) {
                uint32_t nl = de->name_len;
                if (nl > FS_NAME_MAX) nl = FS_NAME_MAX;
                kmemcpy(out[cnt].name, buf + off + FS_DIRENT_HDR, nl);
                out[cnt].name[nl] = 0;
                out[cnt].inode = de->inode;
                out[cnt].type  = de->type;
                cnt++;
            }
            off += rlen;
        }
    }
    return cnt;
}
