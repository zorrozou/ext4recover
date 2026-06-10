/*
 * C7: ghost_dirent.c - recover filenames from pre-deletion dirent remnants
 *
 * On kernels BEFORE commit 6c0912739699 ("ext4: wipe ext4_dir_entry2
 * upon file deletion"), unlinking only absorbed the slot into the
 * previous entry's rec_len; name/inode fields were left on disk.
 * On newer kernels the slot is memset to zero — this code silently
 * gets zero hits on those disks (auto-degrades, no harm).
 *
 * Strategy: walk every allocated directory block; for each "gap"
 * (space between a live entry's end and the next rec_len boundary)
 * try to parse a ghost ext2_dir_entry_2. Any that look valid are
 * added to the filename map via add_filename_mapping(), so the main
 * journal/normal/aggressive paths can attach names to recovered files.
 *
 * Gate: no feature flag needed — the presence/absence of name data
 * is self-evident; empty gaps yield nothing on new kernels.
 * This function adds to the filename map only; it never writes
 * output files and never touches existing recovery results.
 */

#include "ext4_common_v5.h"

/* Heuristic validity check for a candidate ghost dirent at ptr.
 * blocksize is the fs block size; buf_end is one-past-end of valid data. */
static int ghost_dirent_looks_valid(const char *ptr, const char *buf_end,
                                    __u32 max_ino, int blocksize)
{
    if (ptr + 8 > buf_end)           /* need at least fixed header */
        return 0;

    const struct ext2_dir_entry_2 *de = (const struct ext2_dir_entry_2 *)ptr;
    __u32 ino     = de->inode;
    __u16 rec_len = de->rec_len;
    __u8  nlen    = de->name_len;

    if (ino == 0 || ino > max_ino)   return 0;
    if (rec_len < 8 || rec_len > blocksize) return 0;
    if (rec_len % 4 != 0)            return 0;
    if (nlen == 0 || nlen > 255)     return 0;
    if (nlen + 8 > rec_len)          return 0;
    if (ptr + 8 + nlen > buf_end)    return 0;

    /* name must be printable ASCII / valid UTF-8 prefix (no NUL) */
    for (int i = 0; i < nlen; i++) {
        unsigned char c = (unsigned char)de->name[i];
        if (c == 0 || c == '/')
            return 0;
    }
    /* reject . and .. */
    if (nlen == 1 && de->name[0] == '.') return 0;
    if (nlen == 2 && de->name[0] == '.' && de->name[1] == '.') return 0;

    return 1;
}

/* Scan one directory block buf; add ghost names to ctx. */
static void scan_dir_block_for_ghosts(struct recover_context *ctx,
                                      const char *buf, int blocksize,
                                      __u32 max_ino)
{
    int offset = 0;
    while (offset < blocksize) {
        const struct ext2_dir_entry_2 *live =
            (const struct ext2_dir_entry_2 *)(buf + offset);
        if (live->rec_len == 0) break;
        if (offset + live->rec_len > blocksize) break;

        /* gap between real entry end and rec_len boundary */
        int live_used = 8 + live->name_len;
        if (live_used % 4) live_used += 4 - (live_used % 4);
        int gap_start = offset + live_used;
        int gap_end   = offset + live->rec_len;

        /* try to parse ghost(s) in the gap */
        int goff = gap_start;
        while (goff + 8 <= gap_end) {
            if (ghost_dirent_looks_valid(buf + goff, buf + gap_end,
                                         max_ino, blocksize)) {
                const struct ext2_dir_entry_2 *gh =
                    (const struct ext2_dir_entry_2 *)(buf + goff);
                char name[256];
                int nl = gh->name_len;
                memcpy(name, gh->name, nl);
                name[nl] = '\0';
                add_filename_mapping(ctx, gh->inode, name);
                LOG_DEBUG(ctx, "  ghost dirent: ino=%u name=%s",
                         gh->inode, name);
                goff += gh->rec_len;
            } else {
                goff += 4;   /* slide forward by alignment unit */
            }
        }

        offset += live->rec_len;
    }
}

/* Callback for ext2fs_block_iterate3 */
static int ghost_block_cb(ext2_filsys fs, blk64_t *blocknr,
                          e2_blkcnt_t blockcnt __attribute__((unused)),
                          blk64_t ref_blk __attribute__((unused)),
                          int ref_offset __attribute__((unused)),
                          void *priv)
{
    struct recover_context *ctx = priv;
    char *buf;
    if (ext2fs_get_mem(fs->blocksize, &buf) != 0) return 0;
    if (io_channel_read_blk64(fs->io, *blocknr, 1, buf) == 0) {
        scan_dir_block_for_ghosts(ctx, buf, fs->blocksize,
                                   fs->super->s_inodes_count);
    }
    ext2fs_free_mem(&buf);
    return 0;
}

void scan_ghost_dirents(struct recover_context *ctx)
{
    ext2_inode_scan scan;
    ext2_ino_t ino;
    struct ext2_inode inode;
    int added_before = ctx->filename_count;

    LOG_INFO("C7: scanning directory blocks for ghost dirents...");

    if (ext2fs_open_inode_scan(ctx->fs, 0, &scan) != 0) {
        LOG_WARN("C7: failed to open inode scan");
        return;
    }
    while (1) {
        errcode_t r = ext2fs_get_next_inode(scan, &ino, &inode);
        if (r == EXT2_ET_BAD_BLOCK_IN_INODE_TABLE) continue;
        if (r || ino == 0) break;
        if (!LINUX_S_ISDIR(inode.i_mode)) continue;
        if (inode.i_links_count == 0) continue;
        ext2fs_block_iterate3(ctx->fs, ino, BLOCK_FLAG_DATA_ONLY,
                              NULL, ghost_block_cb, ctx);
    }
    ext2fs_close_inode_scan(scan);

    int added = ctx->filename_count - added_before;
    LOG_INFO("C7: ghost dirent scan added %d filename mappings", added);
}
