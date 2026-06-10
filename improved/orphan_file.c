/*
 * C4: orphan_file.c - scan ext4 orphan file blocks (kernel 5.15+)
 *
 * mkfs.ext4 >= 1.47 creates a dedicated orphan file (s_orphan_file_inum)
 * to track unlinked inodes. The s_last_orphan linked-list is then always
 * zero, so the existing orphan_recovery_v5.c path gets nothing.
 *
 * On-disk layout (fs/ext4/ext4.h):
 *   Each block of the orphan file is an array of __le32 inode numbers
 *   followed by struct ext4_orphan_block_tail { ob_magic, ob_checksum }.
 *   Entries of 0 are empty slots.
 *
 * Gate: EXT4_FEATURE_COMPAT_ORPHAN_FILE (ctx->caps.has_orphan_file).
 * Falls back silently to existing s_last_orphan path when flag absent.
 */

#include "ext4_common_v5.h"

#define C4_ORPHAN_BLOCK_MAGIC  0x0b10ca04U

static int inodes_per_orphan_block(struct recover_context *ctx)
{
    return (ctx->blocksize - 8 /* sizeof orphan_block_tail */) /
           sizeof(__u32);
}

int recover_orphan_file(struct recover_context *ctx)
{
    if (!ctx->caps.has_orphan_file) return 0;

    __u32 of_inum = ctx->fs->super->s_orphan_file_inum;
    if (of_inum == 0) return 0;

    struct ext2_inode of_inode;
    if (ext2fs_read_inode(ctx->fs, of_inum, &of_inode) != 0) {
        LOG_WARN("C4: failed to read orphan file inode %u", of_inum);
        return -1;
    }

    __u64 of_size = ((__u64)of_inode.i_size_high << 32) | of_inode.i_size;
    int nblocks = (int)(of_size / ctx->blocksize);
    if (nblocks == 0) return 0;

    int ipob = inodes_per_orphan_block(ctx);
    char *buf;
    if (ext2fs_get_mem(ctx->blocksize, &buf) != 0) return -1;

    int found = 0;
    ext2_file_t of_file;
    if (ext2fs_file_open(ctx->fs, of_inum, 0, &of_file) != 0) {
        ext2fs_free_mem(&buf);
        return -1;
    }

    LOG_INFO("C4: scanning %d orphan file blocks (%d slots each)", nblocks, ipob);

    for (int b = 0; b < nblocks; b++) {
        unsigned int got;
        if (ext2fs_file_llseek(of_file, (__u64)b * ctx->blocksize,
                               EXT2_SEEK_SET, NULL)) continue;
        if (ext2fs_file_read(of_file, buf, ctx->blocksize, &got)) continue;
        if ((int)got != ctx->blocksize) continue;

        /* verify block magic */
        __u32 *tail_magic = (__u32 *)(buf + ctx->blocksize - 8);
        if (ext2fs_le32_to_cpu(*tail_magic) != C4_ORPHAN_BLOCK_MAGIC) {
            LOG_DEBUG(ctx, "C4: block %d bad magic, skip", b);
            continue;
        }

        __u32 *slots = (__u32 *)buf;
        for (int i = 0; i < ipob; i++) {
            __u32 ino = ext2fs_le32_to_cpu(slots[i]);
            if (ino == 0 || ino > ctx->fs->super->s_inodes_count) continue;

            struct ext2_inode inode;
            if (ext2fs_read_inode(ctx->fs, ino, &inode) != 0) continue;
            if (!LINUX_S_ISREG(inode.i_mode)) continue;
            if (!(inode.i_flags & EXT4_EXTENTS_FL)) continue;

            LOG_INFO("C4: orphan file inode %u, attempting recovery", ino);
            int r = recover_from_extent_tree(ctx, ino, &inode);
            if (r == 0) {
                ctx->orphan_recovered++;
                ctx->files_recovered++;
                found++;
            } else {
                ctx->files_failed++;
            }
        }
    }

    ext2fs_file_close(of_file);
    ext2fs_free_mem(&buf);
    LOG_INFO("C4: orphan file scan recovered %d files", found);
    return found;
}
