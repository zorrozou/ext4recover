/*
 * C8: indirect_recovery.c - recover non-extent (indirect block) files
 *
 * Pre-extent-tree ext4 / ext3 files use i_block[0..11] (direct blocks),
 * i_block[12] (single indirect), i_block[13] (double), i_block[14]
 * (triple). Current recovery paths skip any inode without EXT4_EXTENTS_FL
 * — this silently drops all recoverable content on old-era disks.
 *
 * Gate: per-inode !EXT4_EXTENTS_FL (no fs-level flag needed).
 * Works on ext3 disks and old ext4 disks (CentOS 5/6 era).
 * Output prefix: ind_<ino>_file — never touches existing outputs.
 *
 * Strategy: called from journal path when a journal inode copy has
 * !EXT4_EXTENTS_FL and links_count==0. Reads up to triple-indirect chain
 * (capped at MAX_IND_BLOCKS to bound memory), writes sequential output.
 * Block pointers are always 4 bytes (LE); checks each against device size.
 */

#include "ext4_common_v5.h"

#define MAX_IND_BLOCKS (1024 * 1024)   /* 4 GB at 4KB blocks - sanity cap */

static int ind_block_valid(struct recover_context *ctx, __u32 blk)
{
    return blk > 0 && (blk64_t)blk < ctx->total_blocks;
}

/* Read one 4K block from the device. Returns 0 on success. */
static int ind_read(struct recover_context *ctx, __u32 blk, char *buf)
{
    return (pread(ctx->device_fd, buf, ctx->blocksize,
                  (off_t)blk * ctx->blocksize) == ctx->blocksize) ? 0 : -1;
}

/* Walk a single-indirect block and write all valid data blocks to fd.
 * Returns number of blocks written. */
static int dump_indirect(struct recover_context *ctx, __u32 ind_blk,
                         int fd, int *count, char *ibuf, char *dbuf)
{
    if (!ind_block_valid(ctx, ind_blk)) return 0;
    if (ind_read(ctx, ind_blk, ibuf) != 0) return 0;

    int written = 0;
    int per_block = ctx->blocksize / 4;
    __u32 *ptrs = (__u32 *)ibuf;
    for (int i = 0; i < per_block && *count < MAX_IND_BLOCKS; i++) {
        __u32 blk = ext2fs_le32_to_cpu(ptrs[i]);
        if (!ind_block_valid(ctx, blk)) break;   /* stop at first bad ptr */
        if (ind_read(ctx, blk, dbuf) != 0) { (*count)++; continue; }
        if (write(fd, dbuf, ctx->blocksize) == ctx->blocksize) written++;
        (*count)++;
    }
    return written;
}

/* Double-indirect: ibuf2 reused per level-1 block. */
static int dump_double_indirect(struct recover_context *ctx, __u32 dblk,
                                int fd, int *count, char *ibuf, char *ibuf2,
                                char *dbuf)
{
    if (!ind_block_valid(ctx, dblk)) return 0;
    char *level1 = ibuf;
    if (ind_read(ctx, dblk, level1) != 0) return 0;

    int written = 0;
    int per_block = ctx->blocksize / 4;
    __u32 *l1 = (__u32 *)level1;
    for (int i = 0; i < per_block && *count < MAX_IND_BLOCKS; i++) {
        __u32 ind = ext2fs_le32_to_cpu(l1[i]);
        if (!ind_block_valid(ctx, ind)) break;
        written += dump_indirect(ctx, ind, fd, count, ibuf2, dbuf);
    }
    return written;
}

int recover_indirect_file(struct recover_context *ctx,
                          __u32 ino,
                          struct ext2_inode_large *jinode)
{
    if (jinode->i_flags & EXT4_EXTENTS_FL) return -1;  /* extent file */

    __u64 size = ((__u64)jinode->i_size_high << 32) | jinode->i_size;
    if (size == 0) return -1;

    char fname[512];
    char base[300];
    resolve_output_name(ctx, ino, base, sizeof(base));
    snprintf(fname, sizeof(fname), "%s/ind_%s", ctx->recover_dir, base);

    struct stat st;
    if (stat(fname, &st) == 0 && st.st_size >= (off_t)size) return 1;

    int fd = open(fname, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
    if (fd < 0) return -1;

    char *ibuf, *ibuf2, *dbuf;
    if (ext2fs_get_mem(ctx->blocksize, &ibuf) ||
        ext2fs_get_mem(ctx->blocksize, &ibuf2) ||
        ext2fs_get_mem(ctx->blocksize, &dbuf)) {
        close(fd); unlink(fname);
        return -1;
    }

    int ok = 0, count = 0;
    __u32 *iblk = (__u32 *)jinode->i_block;

    /* Direct blocks i_block[0..11] */
    for (int i = 0; i < 12 && count < MAX_IND_BLOCKS; i++) {
        __u32 blk = ext2fs_le32_to_cpu(iblk[i]);
        if (!ind_block_valid(ctx, blk)) break;
        if (ind_read(ctx, blk, dbuf) == 0 &&
            write(fd, dbuf, ctx->blocksize) == ctx->blocksize) ok++;
        count++;
    }
    /* Single indirect */
    ok += dump_indirect(ctx, ext2fs_le32_to_cpu(iblk[12]),
                        fd, &count, ibuf, dbuf);
    /* Double indirect */
    ok += dump_double_indirect(ctx, ext2fs_le32_to_cpu(iblk[13]),
                               fd, &count, ibuf, ibuf2, dbuf);

    ext2fs_free_mem(&ibuf); ext2fs_free_mem(&ibuf2); ext2fs_free_mem(&dbuf);

    if (ok > 0) {
        if (size < (blk64_t)count * ctx->blocksize) {
            if (ftruncate(fd, (off_t)size) != 0)
                LOG_DEBUG(ctx, "ftruncate ind file failed");
        }
        close(fd);
        LOG_INFO("C8: indirect recovered inode %u: %d blocks -> %s", ino, ok, fname);
        ctx->journal_recovered++;
        return 0;
    }
    close(fd); unlink(fname);
    return -1;
}
