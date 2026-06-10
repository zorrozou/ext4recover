/*
 * C1: revoke_scan.c - jbd2 revoke-block targeted recovery
 *
 * When ext4 deletes a file with a multi-level extent tree, it calls
 * __ext4_forget() -> jbd2_journal_revoke() on every freed metadata
 * block (extent leaf/index). These block numbers appear in jbd2
 * REVOKE_BLOCK records (type 5) in the journal — the disk keeps its
 * own "deleted metadata" manifest.
 *
 * C1 mines that manifest and feeds each block into the existing
 * recover_orphaned_extent_block() — the same path aggressive uses,
 * but targeted: O(revoked blocks) vs O(total blocks).
 *
 * Gate: none (jbd2 revoke exists since jbd, works on all era disks).
 * Output prefix: "targeted_<blkno>" (never touches existing outputs).
 * Enable: --targeted flag (default off, like --aggressive).
 */

#include "ext4_common_v5.h"

#define JBD2_REVOKE_BLOCK_TYPE  5

typedef struct {
    __u32 h_magic;
    __u32 h_blocktype;
    __u32 h_sequence;
} c1_hdr_t;

typedef struct {
    c1_hdr_t r_header;
    __u32    r_count;   /* bytes used in this block */
} c1_revoke_hdr_t;

/* How many 32-bit or 64-bit block numbers fit in one revoke block. */
static int revoke_entries_per_block(struct recover_context *ctx, int use64)
{
    int body = ctx->blocksize - (int)sizeof(c1_revoke_hdr_t);
    return body / (use64 ? 8 : 4);
}

int recover_from_revoke(struct recover_context *ctx)
{
    if (!ctx->has_journal || !ctx->journal_file) {
        LOG_INFO("--targeted: no journal, skip");
        return 0;
    }

    int use64 = ctx->caps.journal_probed ? ctx->caps.jbd2_has_64bit : 0;
    char *buf;
    if (ext2fs_get_mem(ctx->blocksize, &buf) != 0) return -1;
    char *blkbuf;
    if (ext2fs_get_mem(ctx->blocksize, &blkbuf) != 0) {
        ext2fs_free_mem(&buf);
        return -1;
    }

    int found = 0, examined = 0;
    unsigned int got;

    for (blk64_t jblk = ctx->journal_start; jblk < ctx->journal_len; jblk++) {
        if (ext2fs_file_llseek(ctx->journal_file,
                               (__u64)jblk * ctx->blocksize,
                               EXT2_SEEK_SET, NULL)) continue;
        if (ext2fs_file_read(ctx->journal_file, buf, ctx->blocksize, &got))
            continue;
        if ((int)got != ctx->blocksize) continue;

        c1_revoke_hdr_t *rh = (c1_revoke_hdr_t *)buf;
        if (__builtin_bswap32(rh->r_header.h_magic) != 0xc03b3998U) continue;
        if (__builtin_bswap32(rh->r_header.h_blocktype) != JBD2_REVOKE_BLOCK_TYPE) continue;

        int r_count = (int)__builtin_bswap32(rh->r_count);
        int body_bytes = r_count - (int)sizeof(c1_revoke_hdr_t);
        if (body_bytes <= 0) continue;

        char *p = buf + sizeof(c1_revoke_hdr_t);
        char *end = buf + r_count;
        while (p + (use64 ? 8 : 4) <= end) {
            blk64_t rblk;
            if (use64) {
                __u32 hi = __builtin_bswap32(*(__u32 *)p);
                __u32 lo = __builtin_bswap32(*(__u32 *)(p + 4));
                rblk = ((blk64_t)hi << 32) | lo;
                p += 8;
            } else {
                rblk = __builtin_bswap32(*(__u32 *)p);
                p += 4;
            }
            if (rblk < 2 || rblk >= ctx->total_blocks) continue;
            examined++;

            /* Try journal version first (B2), then disk */
            int got_block = 0;
            if (journal_index_read(ctx, rblk, 0, blkbuf) > 0) {
                struct ext3_extent_header *eh = (struct ext3_extent_header *)blkbuf;
                if (ext2fs_le16_to_cpu(eh->eh_magic) == EXT4_EXT_MAGIC)
                    got_block = 1;
            }
            if (!got_block) {
                if (pread(ctx->device_fd, blkbuf, ctx->blocksize,
                          (off_t)rblk * ctx->blocksize) == ctx->blocksize) {
                    struct ext3_extent_header *eh = (struct ext3_extent_header *)blkbuf;
                    if (ext2fs_le16_to_cpu(eh->eh_magic) == EXT4_EXT_MAGIC)
                        got_block = 1;
                }
            }
            if (!got_block) continue;

            /* Use aggressive path but write to targeted_* prefix */
            char orig_dir[256];
            strncpy(orig_dir, ctx->recover_dir, sizeof(orig_dir) - 1);
            orig_dir[sizeof(orig_dir) - 1] = '\0';

            char tgt_dir[512];
            snprintf(tgt_dir, sizeof(tgt_dir), "%s", ctx->recover_dir);
            /* We reuse recover_orphaned_extent_block which writes to
             * recover_dir/aggressive_* — temporarily patch the filename
             * by writing directly to avoid touching existing outputs.    */
            /* Instead: generate the output name ourselves and only call
             * the block if not already recovered. */
            if (is_block_recovered(rblk)) continue;

            /* Emit targeted_<rblk> file */
            char filename[512];
            snprintf(filename, sizeof(filename), "%s/targeted_%llu",
                     ctx->recover_dir, (unsigned long long)rblk);
            struct stat st;
            if (stat(filename, &st) == 0 && st.st_size > 0) continue;
            int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
            if (fd < 0) continue;

            ctx->recover_fd = fd;
            /* walk the extent block directly */
            int ok = 0;
            struct ext3_extent_header *eh = (struct ext3_extent_header *)blkbuf;
            if (ext2fs_le16_to_cpu(eh->eh_depth) == 0) {
                /* leaf: dump extents */
                struct ext3_extent *ee = EXT_FIRST_EXTENT(eh);
                int n = ext2fs_le16_to_cpu(eh->eh_entries);
                for (int i = 0; i < n; i++, ee++) {
                    __u16 elen = ext2fs_le16_to_cpu(ee->ee_len);
                    __u64 estart = (((__u64)ext2fs_le16_to_cpu(ee->ee_start_hi) << 32) +
                                   (__u64)ext2fs_le32_to_cpu(ee->ee_start));
                    if (elen > 32768) elen -= 32768;
                    if (elen == 0 || estart == 0) continue;
                    if (estart + elen > ctx->total_blocks) continue;
                    if (recover_block_to_file(ctx->device_fd, fd,
                                             ext2fs_le32_to_cpu(ee->ee_block),
                                             elen, estart,
                                             ctx->blocksize, ctx))
                        ok++;
                }
            } else {
                /* index node: use existing tree walker */
                ok = recover_orphaned_extent_block(ctx, rblk, blkbuf);
                /* recover_orphaned_extent_block already opens/closes its own
                 * file; close our fd since it won't be written. */
                close(fd);
                if (ok > 0) {
                    found++;
                    ctx->aggressive_recovered++;
                }
                continue;
            }
            close(fd);
            if (ok > 0) {
                LOG_INFO("Targeted recover revoked block %llu -> %s (%d extents)",
                         (unsigned long long)rblk, filename, ok);
                found++;
                ctx->aggressive_recovered++;
            } else {
                unlink(filename);
            }
        }
    }

    ext2fs_free_mem(&blkbuf);
    ext2fs_free_mem(&buf);
    LOG_INFO("Targeted scan complete: examined %d revoked blocks, recovered %d",
             examined, found);
    return found;
}
