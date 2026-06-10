/*
 * C6: inline_data.c - recover files stored inline in the inode
 *
 * When EXT4_INLINE_DATA_FL is set, small files (<= 60 bytes) store
 * their data in i_block[0..14] (60 bytes), and slightly larger ones
 * also use the ea_inode / inline xattr body. The journal holds old
 * inode table blocks that may still carry the pre-delete inode with
 * the inline data intact.
 *
 * This module is called from recover_inode_from_journal() when the
 * journal inode copy has EXT4_INLINE_DATA_FL set and links_count==0.
 * It simply copies i_block[] (up to i_size bytes) into the output.
 *
 * Gate: EXT4_FEATURE_INCOMPAT_INLINE_DATA (ctx->caps.has_inline_data).
 * Called per-inode from journal scan, zero cost when flag absent.
 */

#include "ext4_common_v5.h"

#ifndef EXT4_INLINE_DATA_FL
#define EXT4_INLINE_DATA_FL  0x10000000
#endif
#define EXT4_INLINE_MAX_BYTES 60   /* sizeof(i_block) */

int recover_inline_data(struct recover_context *ctx,
                        __u32 ino,
                        struct ext2_inode_large *jinode)
{
    if (!ctx->caps.has_inline_data) return -1;
    if (!(jinode->i_flags & EXT4_INLINE_DATA_FL)) return -1;

    __u64 size = ((__u64)jinode->i_size_high << 32) | jinode->i_size;
    if (size == 0 || size > EXT4_INLINE_MAX_BYTES) return -1;

    char filename[512];
    char base[300];
    resolve_output_name(ctx, ino, base, sizeof(base));
    snprintf(filename, sizeof(filename), "%s/inline_%s", ctx->recover_dir, base);

    /* don't overwrite a better (larger) existing file */
    struct stat st;
    if (stat(filename, &st) == 0 && st.st_size >= (off_t)size) return 1;

    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
    if (fd < 0) return -1;

    ssize_t written = write(fd, jinode->i_block, (size_t)size);
    close(fd);

    if (written != (ssize_t)size) {
        unlink(filename);
        return -1;
    }

    LOG_INFO("C6: inline data recovered inode %u: %llu bytes -> %s",
             ino, (unsigned long long)size, filename);
    ctx->journal_recovered++;
    return 0;
}
