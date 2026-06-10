/*
 * aggressive_scan.c - Full-disk extent header scanning
 * 
 * Scans all blocks on the device looking for extent headers (eh_magic = 0xF30A).
 * This can recover orphaned extent blocks that are no longer referenced by
 * any inode but still contain valid data.
 * 
 * Optimizations:
 * - Use block bitmap to skip already-allocated blocks
 * - Batch reads for better I/O performance
 * - Track already-recovered blocks to avoid duplicates
 */

#include "ext4_common_v5.h"
#include "parallel_scan.h"

/* B4: recovered-block set as an open-addressing hash (was a linked
 * list giving O(hits) per is_block_recovered query, called once per
 * scanned block - billions of times on a multi-TB device).
 * Key 0 = empty slot (block 0 is the superblock, never a candidate). */
static blk64_t *rb_keys = NULL;
static int rb_size = 0;        /* power of two */
static int rb_count = 0;

static int rb_grow(int min_size)
{
    int ns = 4096;
    while (ns < min_size * 2) ns <<= 1;
    blk64_t *nk = calloc(ns, sizeof(blk64_t));
    if (!nk) return -1;
    for (int i = 0; i < rb_size; i++) {
        if (rb_keys && rb_keys[i]) {
            __u64 h = (rb_keys[i] * 0x9E3779B97F4A7C15ULL) & (__u64)(ns - 1);
            while (nk[h]) h = (h + 1) & (ns - 1);
            nk[h] = rb_keys[i];
        }
    }
    free(rb_keys);
    rb_keys = nk;
    rb_size = ns;
    return 0;
}

/*
 * Check if a block was already recovered
 */
int is_block_recovered(blk64_t block)
{
    if (!rb_size || block == 0) return 0;
    __u64 h = (block * 0x9E3779B97F4A7C15ULL) & (__u64)(rb_size - 1);
    while (rb_keys[h]) {
        if (rb_keys[h] == block)
            return 1;
        h = (h + 1) & (__u64)(rb_size - 1);
    }
    return 0;
}

/*
 * Mark a block as recovered
 */
static void mark_block_recovered(blk64_t block)
{
    if (block == 0) return;
    if (rb_count * 2 >= rb_size) {
        if (rb_grow(rb_count + 1) != 0)
            return;   /* OOM: dedup degrades, correctness preserved */
    }
    __u64 h = (block * 0x9E3779B97F4A7C15ULL) & (__u64)(rb_size - 1);
    while (rb_keys[h]) {
        if (rb_keys[h] == block)
            return;
        h = (h + 1) & (__u64)(rb_size - 1);
    }
    rb_keys[h] = block;
    rb_count++;
}

/*
 * Free recovered block set
 */
static void free_recovered_list(void)
{
    free(rb_keys);
    rb_keys = NULL;
    rb_size = 0;
    rb_count = 0;
}

/*
 * Check if a block is free in the block bitmap
 */
static int is_block_free(struct recover_context *ctx, blk64_t block)
{
    /* If no block bitmap loaded, assume we should check */
    if (!ctx->fs->block_map)
        return 1;
    
    return !ext2fs_fast_test_block_bitmap2(ctx->fs->block_map, block);
}

/*
 * Phase 4: depth>0 file-level reconstruction.
 *
 * Aggressively rebuild a multi-level extent tree starting from a root/index
 * block at block_num (with buf holding its already-read content). Walks
 * ei_leaf_hi:ei_leaf links down to all leaves, collects all leaf extents,
 * sorts by logical block, and dumps the entire reconstructed file as one
 * recovery output instead of N independent leaf fragments.
 *
 * Returns 1 if a file was emitted, 0 otherwise.
 *
 * Safety:
 *   - ei_leaf must be in [2, total_blocks) (skip otherwise)
 *   - each child block must have a valid extent header (magic + sane entries)
 *   - max tree depth bounded to MAX_TREE_DEPTH
 *   - all candidate extents are validated against device size before dump
 */
#define MAX_TREE_DEPTH      5
#define MAX_COLLECTED_EXT   65536  /* hard cap to bound memory */

struct collected_extent {
    __u32 ee_block;     /* logical block in file */
    __u32 ee_len;       /* length in blocks (uninit flag cleared) */
    __u64 ee_start;     /* physical block on device */
    int   uninit;       /* 1 if was uninitialized */
};

static int collected_ext_cmp(const void *a, const void *b)
{
    const struct collected_extent *ea = a, *eb = b;
    if (ea->ee_block < eb->ee_block) return -1;
    if (ea->ee_block > eb->ee_block) return 1;
    return 0;
}

/* Recursively walk a depth>=1 extent header block. buf is its content.
 * Appends every leaf extent into out[] (up to *out_cap), updating *out_n.
 * Returns 0 on success (tree consistent), -1 on fatal inconsistency
 * (caller should abandon the whole tree). */
static int walk_extent_tree(struct recover_context *ctx,
                            blk64_t this_block,
                            char *buf,
                            int depth_left,
                            struct collected_extent *out,
                            int *out_n,
                            int out_cap)
{
    struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
    
    if (depth_left <= 0) {
        LOG_DEBUG(ctx, "  tree walk: max depth reached at block %llu",
                 (unsigned long long)this_block);
        return -1;
    }
    
    if (!validate_extent_header(eh, ctx->blocksize)) {
        LOG_DEBUG(ctx, "  tree walk: invalid header at block %llu",
                 (unsigned long long)this_block);
        return -1;
    }
    
    int entries = ext2fs_le16_to_cpu(eh->eh_entries);
    int this_depth = ext2fs_le16_to_cpu(eh->eh_depth);
    
    if (this_depth == 0) {
        /* Leaf: append every extent */
        struct ext3_extent *ee = EXT_FIRST_EXTENT(eh);
        for (int i = 0; i < entries; i++, ee++) {
            __u16 raw_len = ext2fs_le16_to_cpu(ee->ee_len);
            __u32 ee_block = ext2fs_le32_to_cpu(ee->ee_block);
            __u64 ee_start = (((__u64)ext2fs_le16_to_cpu(ee->ee_start_hi) << 32) +
                              (__u64)ext2fs_le32_to_cpu(ee->ee_start));
            int uninit = 0;
            __u32 use_len = raw_len;
            if (raw_len > 32768) { uninit = 1; use_len = raw_len - 32768; }
            if (use_len == 0 || ee_start == 0) continue;
            if (ee_start + use_len > ctx->total_blocks) continue;
            if (*out_n >= out_cap) {
                LOG_WARN("  tree walk: collected extent cap %d reached", out_cap);
                return -1;
            }
            out[*out_n].ee_block = ee_block;
            out[*out_n].ee_len   = use_len;
            out[*out_n].ee_start = ee_start;
            out[*out_n].uninit   = uninit;
            (*out_n)++;
        }
        return 0;
    }
    
    /* Index node: follow each ei_leaf */
    struct ext3_extent_idx *ei = EXT_FIRST_INDEX(eh);
    char *child_buf;
    errcode_t r = ext2fs_get_mem(ctx->blocksize, &child_buf);
    if (r) return -1;
    
    int ret = 0;
    for (int i = 0; i < entries; i++, ei++) {
        __u64 leaf_blk = (((__u64)ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32) +
                          (__u64)ext2fs_le32_to_cpu(ei->ei_leaf));
        if (leaf_blk < 2 || leaf_blk >= ctx->total_blocks) {
            LOG_DEBUG(ctx, "  tree walk: ei_leaf %llu out of range",
                     (unsigned long long)leaf_blk);
            continue;  /* skip suspicious idx but try others */
        }
        errcode_t rr = io_channel_read_blk64(ctx->fs->io, leaf_blk, 1, child_buf);
        if (rr) {
            LOG_DEBUG(ctx, "  tree walk: read fail at leaf %llu",
                     (unsigned long long)leaf_blk);
            continue;
        }
        /* Quick magic check before recursing */
        struct ext3_extent_header *ch = (struct ext3_extent_header *)child_buf;
        if (ext2fs_le16_to_cpu(ch->eh_magic) != EXT4_EXT_MAGIC) {
            LOG_DEBUG(ctx, "  tree walk: bad magic at leaf %llu",
                     (unsigned long long)leaf_blk);
            continue;
        }
        /* Mark child block as recovered so the linear scan won't re-emit
         * it as an independent aggressive_<leaf> file. */
        mark_block_recovered((blk64_t)leaf_blk);
        
        if (walk_extent_tree(ctx, (blk64_t)leaf_blk, child_buf,
                             depth_left - 1, out, out_n, out_cap) < 0) {
            /* one child failed, but we still try to gather what we can
             * from siblings; only abort whole tree if collected nothing. */
            continue;
        }
    }
    ext2fs_free_mem(&child_buf);
    return ret;
}

int recover_orphaned_extent_tree(struct recover_context *ctx,
                                 blk64_t root_block,
                                 char *root_buf);
                                 
int recover_orphaned_extent_tree(struct recover_context *ctx,
                                 blk64_t root_block,
                                 char *root_buf)
{
    struct ext3_extent_header *eh = (struct ext3_extent_header *)root_buf;
    int root_depth = ext2fs_le16_to_cpu(eh->eh_depth);
    
    LOG_INFO("Found orphaned extent tree root at %llu, depth=%d entries=%u",
            (unsigned long long)root_block,
            root_depth,
            ext2fs_le16_to_cpu(eh->eh_entries));
    
    struct collected_extent *exts = calloc(MAX_COLLECTED_EXT,
                                            sizeof(struct collected_extent));
    if (!exts) {
        LOG_ERROR("Failed to alloc collected_extent array");
        return 0;
    }
    int n = 0;
    
    if (walk_extent_tree(ctx, root_block, root_buf,
                         MAX_TREE_DEPTH, exts, &n, MAX_COLLECTED_EXT) < 0
        && n == 0) {
        LOG_DEBUG(ctx, "  tree walk failed completely, nothing to dump");
        free(exts);
        return 0;
    }
    
    if (n == 0) {
        LOG_DEBUG(ctx, "  tree at %llu yielded 0 extents",
                 (unsigned long long)root_block);
        free(exts);
        return 0;
    }
    
    /* Sort by logical block */
    qsort(exts, n, sizeof(struct collected_extent), collected_ext_cmp);
    
    /* Dedup pre-check: if every extent is already covered, skip whole file */
    if (ctx->recovered_extents) {
        int fully = 0;
        for (int k = 0; k < n; k++) {
            if (intervals_query(ctx->recovered_extents,
                                (uint64_t)exts[k].ee_start,
                                (uint64_t)exts[k].ee_len) == 1)
                fully++;
        }
        if (fully == n) {
            LOG_DEBUG(ctx, "  aggressive tree %llu fully covered; skip",
                     (unsigned long long)root_block);
            mark_block_recovered(root_block);
            free(exts);
            return 0;
        }
    }
    
    /* Emit one file containing all extents in logical order */
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/aggressive_tree_%llu",
            ctx->recover_dir, (unsigned long long)root_block);
    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
    if (fd < 0) {
        LOG_ERROR("Failed to create recovery file: %s", filename);
        free(exts);
        return 0;
    }
    
    int recovered = 0;
    for (int k = 0; k < n; k++) {
        if (exts[k].uninit) {
            /* skip uninitialized regions: not real data */
            continue;
        }
        LOG_DEBUG(ctx, "  tree extent %d: logical=%u len=%u phys=%llu",
                 k, exts[k].ee_block, exts[k].ee_len,
                 (unsigned long long)exts[k].ee_start);
        if (recover_block_to_file(ctx->device_fd, fd,
                                  exts[k].ee_block, exts[k].ee_len,
                                  exts[k].ee_start, ctx->blocksize, ctx)) {
            recovered++;
        }
    }
    close(fd);
    
    if (recovered > 0) {
        LOG_INFO("Reconstructed tree at %llu into %s (%d extents)",
                (unsigned long long)root_block, filename, recovered);
        mark_block_recovered(root_block);
        ctx->aggressive_recovered++;
        free(exts);
        return 1;
    }
    
    unlink(filename);
    free(exts);
    return 0;
}

/*
 * Try to recover data from an orphaned extent leaf block
 */
int recover_orphaned_extent_block(struct recover_context *ctx,
                                  blk64_t block_num,
                                  char *buf)
{
    struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
    struct ext3_extent *ee;
    char filename[256];
    int fd, i;
    int recovered = 0;
    
    /* Additional validation first - applies to both leaf and index */
    if (!validate_extent_header(eh, ctx->blocksize))
        return 0;
    
    /* Phase 4: handle multi-level extent tree (depth>=1).
     * Follow ei_leaf chain, collect all leaf extents, emit one file. */
    if (eh->eh_depth != 0) {
        return recover_orphaned_extent_tree(ctx, block_num, buf);
    }
    
    LOG_INFO("Found orphaned extent leaf block at %llu, entries=%u",
            (unsigned long long)block_num,
            ext2fs_le16_to_cpu(eh->eh_entries));
    
    /* Pre-pass: if every extent in this leaf is already covered by a
     * previous recovery (normal/journal/another aggressive hit), this
     * leaf is fully redundant and we skip creating a file at all. */
    if (ctx->recovered_extents) {
        struct ext3_extent *pe = EXT_FIRST_EXTENT(eh);
        int neh = ext2fs_le16_to_cpu(eh->eh_entries);
        int total = 0, fully = 0;
        for (int k = 0; k < neh; k++, pe++) {
            __u16 plen = ext2fs_le16_to_cpu(pe->ee_len);
            __u64 pst  = (((__u64)ext2fs_le16_to_cpu(pe->ee_start_hi) << 32) +
                          (__u64)ext2fs_le32_to_cpu(pe->ee_start));
            if (plen > 32768) plen -= 32768; /* uninit */
            if (plen == 0 || pst == 0) continue;
            total++;
            if (intervals_query(ctx->recovered_extents,
                                (uint64_t)pst, (uint64_t)plen) == 1)
                fully++;
        }
        if (total > 0 && fully == total) {
            LOG_DEBUG(ctx, "  aggressive leaf %llu fully covered; skip",
                     (unsigned long long)block_num);
            mark_block_recovered(block_num);
            return 0;
        }
    }
    
    /* Create recovery file */
    snprintf(filename, sizeof(filename), "%s/aggressive_%llu",
            ctx->recover_dir, (unsigned long long)block_num);
    
    fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
    if (fd < 0) {
        LOG_ERROR("Failed to create recovery file: %s", filename);
        return -1;
    }
    
    /* Extract all extents */
    ee = EXT_FIRST_EXTENT(eh);
    for (i = 0; i < ext2fs_le16_to_cpu(eh->eh_entries); i++, ee++) {
        __le32 ee_block = ext2fs_le32_to_cpu(ee->ee_block);
        __le16 ee_len = ext2fs_le16_to_cpu(ee->ee_len);
        __u64 ee_start = (((__u64)ext2fs_le16_to_cpu(ee->ee_start_hi) << 32) +
                         (__u64)ext2fs_le32_to_cpu(ee->ee_start));
        
        /* Skip invalid extents */
        if (ee_len == 0 || ee_start == 0)
            continue;
        
        /* Validate extent is within device */
        if (ee_start + ee_len > ctx->total_blocks) {
            LOG_WARN("Extent %d in block %llu exceeds device size, skipping",
                    i, (unsigned long long)block_num);
            continue;
        }
        
        LOG_DEBUG(ctx, "  Recovering extent %d: logical=%u, len=%u, physical=%llu",
                 i, ee_block, ee_len, (unsigned long long)ee_start);
        
        /* Recover data */
        if (recover_block_to_file(ctx->device_fd, fd, ee_block, ee_len,
                                 ee_start, ctx->blocksize, ctx)) {
            recovered++;
        }
    }
    
    close(fd);
    
    if (recovered > 0) {
        LOG_INFO("Recovered %d extents from block %llu to %s",
                recovered, (unsigned long long)block_num, filename);
        mark_block_recovered(block_num);
        ctx->aggressive_recovered++;
        return 1;
    } else {
        /* No data recovered, remove empty file */
        unlink(filename);
        return 0;
    }
}

/*
 * Scan a range of blocks for extent headers
 */
int scan_for_extent_headers(struct recover_context *ctx, blk64_t start,
                            blk64_t end)
{
    char *buf;
    blk64_t block;
    errcode_t retval;
    int found = 0;
    
    /* Allocate read buffer */
    retval = ext2fs_get_mem(ctx->blocksize, &buf);
    if (retval) {
        LOG_ERROR("Failed to allocate scan buffer");
        return -1;
    }
    
    LOG_INFO("Scanning blocks %llu to %llu for extent headers",
            (unsigned long long)start, (unsigned long long)end);
    
    for (block = start; block < end; block++) {
        /* Progress report every 10000 blocks */
        if ((block - start) % 10000 == 0 && block > start) {
            LOG_INFO("Scanned %llu blocks, found %d orphaned extent blocks",
                    (unsigned long long)(block - start), found);
        }
        
        /* Skip if already recovered */
        if (is_block_recovered(block))
            continue;
        
        /* Skip if block is currently allocated (optional optimization) */
        /* Commented out because free blocks might contain old extents */
        /* if (!is_block_free(ctx, block))
            continue; */
        
        /* Read block */
        retval = io_channel_read_blk64(ctx->fs->io, block, 1, buf);
        if (retval) {
            /* I/O error, skip this block */
            continue;
        }
        
        /* Check for extent header magic */
        struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
        if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT4_EXT_MAGIC)
            continue;
        
        LOG_DEBUG(ctx, "Found extent header at block %llu",
                 (unsigned long long)block);
        
        /* Try to recover from this extent block */
        if (recover_orphaned_extent_block(ctx, block, buf) > 0)
            found++;
    }
    
    ext2fs_free_mem(&buf);
    
    LOG_INFO("Scan complete: found %d orphaned extent blocks in range", found);
    return found;
}

/*
 * Perform aggressive full-disk scan
 */
int aggressive_scan(struct recover_context *ctx)
{
    blk64_t start_block, end_block;
    int retval;
    
    LOG_INFO("Starting aggressive full-disk scan...");
    LOG_INFO("This may take a long time for large devices");
    
    /* Load block bitmap if not already loaded */
    if (!ctx->fs->block_map) {
        LOG_INFO("Loading block bitmap...");
        retval = ext2fs_read_block_bitmap(ctx->fs);
        if (retval) {
            LOG_WARN("Failed to load block bitmap, scanning all blocks");
        }
    }
    
    /* Determine scan range */
    start_block = ctx->fs->super->s_first_data_block;
    end_block = ext2fs_blocks_count(ctx->fs->super);
    ctx->total_blocks = end_block;
    
    LOG_INFO("Device has %llu total blocks (%llu MB)",
            (unsigned long long)end_block,
            (unsigned long long)(end_block * ctx->blocksize / 1024 / 1024));
    
    /* Scan the device (parallel by default, fallback to serial) */
    if (ctx->use_parallel) {
        int nw = ctx->n_workers;
        if (nw <= 0) {
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            nw = (ncpu > 1) ? (int)(ncpu - 1) : 1; /* leave 1 core for reader/writer */
        }
        retval = parallel_scan_for_extent_headers(ctx, start_block, end_block, nw);
        if (retval < 0) {
            LOG_WARN("Parallel scan setup failed, falling back to serial");
            retval = scan_for_extent_headers(ctx, start_block, end_block);
        }
    } else {
        retval = scan_for_extent_headers(ctx, start_block, end_block);
    }
    
    /* Clean up */
    free_recovered_list();
    
    if (retval < 0) {
        LOG_ERROR("Aggressive scan failed");
        return retval;
    }
    
    LOG_INFO("Aggressive scan complete: recovered %d extent blocks",
            retval);
    return 0;
}
