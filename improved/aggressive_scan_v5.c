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

/* Track recovered extent blocks to avoid duplicates */
struct recovered_extent_block {
    blk64_t block_num;
    struct recovered_extent_block *next;
};

static struct recovered_extent_block *recovered_list = NULL;

/*
 * Check if a block was already recovered
 */
static int is_block_recovered(blk64_t block)
{
    struct recovered_extent_block *p = recovered_list;
    
    while (p) {
        if (p->block_num == block)
            return 1;
        p = p->next;
    }
    
    return 0;
}

/*
 * Mark a block as recovered
 */
static void mark_block_recovered(blk64_t block)
{
    struct recovered_extent_block *p;
    
    p = malloc(sizeof(struct recovered_extent_block));
    if (!p)
        return;
    
    p->block_num = block;
    p->next = recovered_list;
    recovered_list = p;
}

/*
 * Free recovered block list
 */
static void free_recovered_list(void)
{
    struct recovered_extent_block *p, *next;
    
    p = recovered_list;
    while (p) {
        next = p->next;
        free(p);
        p = next;
    }
    recovered_list = NULL;
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
 * Try to recover data from an orphaned extent leaf block
 */
static int recover_orphaned_extent_block(struct recover_context *ctx,
                                         blk64_t block_num,
                                         char *buf)
{
    struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
    struct ext3_extent *ee;
    char filename[256];
    int fd, i;
    int recovered = 0;
    
    /* Validate it's a leaf block */
    if (eh->eh_depth != 0) {
        LOG_DEBUG(ctx, "Block %llu is not a leaf extent block, skipping",
                 (unsigned long long)block_num);
        return 0;
    }
    
    /* Additional validation */
    if (!validate_extent_header(eh, ctx->blocksize))
        return 0;
    
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
    
    /* Scan the device */
    retval = scan_for_extent_headers(ctx, start_block, end_block);
    
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

/*
 * Scan only free blocks (optimization)
 * This is faster but may miss recently-freed extents
 */
int aggressive_scan_free_blocks(struct recover_context *ctx)
{
    blk64_t block;
    errcode_t retval;
    char *buf;
    int found = 0;
    
    LOG_INFO("Starting aggressive scan of free blocks only...");
    
    /* Load block bitmap */
    if (!ctx->fs->block_map) {
        retval = ext2fs_read_block_bitmap(ctx->fs);
        if (retval) {
            LOG_ERROR("Failed to load block bitmap");
            return -1;
        }
    }
    
    /* Allocate read buffer */
    retval = ext2fs_get_mem(ctx->blocksize, &buf);
    if (retval) {
        LOG_ERROR("Failed to allocate scan buffer");
        return -1;
    }
    
    /* Iterate through all blocks */
    for (block = ctx->fs->super->s_first_data_block;
         block < ext2fs_blocks_count(ctx->fs->super);
         block++) {
        
        /* Only check free blocks */
        if (!is_block_free(ctx, block))
            continue;
        
        /* Already recovered? */
        if (is_block_recovered(block))
            continue;
        
        /* Progress report */
        if (found > 0 && found % 100 == 0) {
            LOG_INFO("Found %d orphaned extent blocks so far...", found);
        }
        
        /* Read and check block */
        retval = io_channel_read_blk64(ctx->fs->io, block, 1, buf);
        if (retval)
            continue;
        
        struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
        if (ext2fs_le16_to_cpu(eh->eh_magic) == EXT4_EXT_MAGIC) {
            if (recover_orphaned_extent_block(ctx, block, buf) > 0)
                found++;
        }
    }
    
    ext2fs_free_mem(&buf);
    free_recovered_list();
    
    LOG_INFO("Free block scan complete: found %d extent blocks", found);
    return 0;
}
