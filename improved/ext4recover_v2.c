/*
 * ext4recover_v2.c - Enhanced ext4 file recovery tool
 * 
 * Enhanced version with multiple recovery strategies:
 * - Orphan list priority recovery
 * - Journal (jbd2) parsing for deleted extent blocks
 * - Aggressive full-disk scanning
 * - Dynamic eh_max calculation and deduplication
 * 
 * Based on original work by zorrozou and curuwang
 */

#include "ext4_common.h"

/* Global context */
static struct recover_context g_ctx;

/*
 * Utility: Check if recovery directory is on target device
 */
int is_on_device(const char *path, const char *dev)
{
    struct stat stat1, stat2;
    int ret;
    
    ret = stat(path, &stat1);
    if (ret < 0) {
        perror("stat");
        return -1;
    }
    
    ret = stat(dev, &stat2);
    if (ret < 0) {
        perror("stat");
        return -1;
    }
    
    if (((stat2.st_mode & S_IFMT) == S_IFBLK) && 
        (stat1.st_dev == stat2.st_rdev)) {
        return 1;
    }
    
    return 0;
}

/*
 * Utility: Recover blocks to file
 */
int recover_block_to_file(int devfd, int inofd, __le32 block, __le16 len,
                         __u64 start, ssize_t blocksize)
{
    off_t offset_dev, offset_ino;
    ssize_t got, ret;
    char *buf;
    int i;
    
    buf = malloc(blocksize);
    if (!buf) {
        LOG_ERROR("Failed to allocate block buffer");
        return 0;
    }
    
    offset_dev = lseek(devfd, (off_t)start * blocksize, SEEK_SET);
    if (offset_dev < 0) {
        perror("lseek(devfd)");
        free(buf);
        return 0;
    }
    
    offset_ino = lseek(inofd, (off_t)block * blocksize, SEEK_SET);
    if (offset_ino < 0) {
        perror("lseek(inofd)");
        free(buf);
        return 0;
    }
    
    for (i = 0; i < len; i++) {
        ssize_t off = 0;
        
        /* Read full block from device */
        while (off < blocksize) {
            got = read(devfd, buf + off, blocksize - off);
            if (got < 0) {
                perror("read(devfd)");
                free(buf);
                return 0;
            }
            if (got == 0) {
                LOG_ERROR("Short read on device at extent block %llu",
                         (unsigned long long)start);
                free(buf);
                return 0;
            }
            off += got;
        }
        
        /* Write to output file */
        ssize_t written = 0;
        while (written < blocksize) {
            ret = write(inofd, buf + written, blocksize - written);
            if (ret < 0) {
                perror("write(inofd)");
                free(buf);
                return 0;
            }
            if (ret == 0) {
                LOG_ERROR("Write returned 0 (disk full?)");
                free(buf);
                return 0;
            }
            written += ret;
        }
    }
    
    free(buf);
    return 1;
}

/*
 * Create recovery file for an inode
 */
int create_recovery_file(struct recover_context *ctx, __u32 ino, int *fd_out)
{
    char filename[256];
    
    snprintf(filename, sizeof(filename), "%s/%u_file", 
            ctx->recover_dir, ino);
    
    /* Don't overwrite files already recovered by journal */
    struct stat existing;
    if (stat(filename, &existing) == 0 && existing.st_size > 0) {
        *fd_out = -1;
        return -2;  /* Already recovered, skip */
    }
    
    *fd_out = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
    if (*fd_out < 0) {
        LOG_ERROR("Failed to create recovery file for inode %u", ino);
        return -1;
    }
    
    return 0;
}

/*
 * Dump extent entries from a leaf block
 */
static int dump_leaf_extent(struct recover_context *ctx,
                           struct ext3_extent_header *eh)
{
    struct ext3_extent *ee;
    int i, retval;
    __le32 ee_block;
    __le16 ee_len;
    __u64 ee_start;
    
    /* Validate using dynamic eh_max */
    int max_entries = calculate_max_entries(ctx->blocksize, 1);
    
    if (ext2fs_le16_to_cpu(eh->eh_entries) > max_entries) {
        LOG_WARN("eh_entries (%u) exceeds calculated max (%d)",
                ext2fs_le16_to_cpu(eh->eh_entries), max_entries);
        return 1;
    }
    
    if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT4_EXT_MAGIC) {
        return 1;
    }
    
    ee = EXT_FIRST_EXTENT(eh);
    for (i = 0; i < ext2fs_le16_to_cpu(eh->eh_entries); i++, ee++) {
        ee_block = ext2fs_le32_to_cpu(ee->ee_block);
        ee_len = ext2fs_le16_to_cpu(ee->ee_len);
        ee_start = (((__u64)ext2fs_le16_to_cpu(ee->ee_start_hi) << 32) +
                   (__u64)ext2fs_le32_to_cpu(ee->ee_start));
        
        /* Skip invalid extents */
        if (ee_len == 0 || ee_start == 0)
            continue;
        
        LOG_DEBUG(ctx, "  Extent: logical=%u, len=%u, physical=%llu",
                 ee_block, ee_len, (unsigned long long)ee_start);
        
        retval = recover_block_to_file(ctx->device_fd, ctx->recover_fd,
                                      ee_block, ee_len, ee_start,
                                      ctx->blocksize);
        if (!retval) {
            LOG_ERROR("Failed to recover extent");
            return 0;
        }
    }
    
    return 1;
}

/*
 * Recursively travel extent tree
 */
static int extent_tree_travel(struct recover_context *ctx,
                             ext2_extent_handle_t handle,
                             struct ext3_extent_header *eh)
{
    struct ext3_extent_header *next;
    struct ext3_extent_idx *ei;
    int i, retval;
    char *buf = NULL;
    blk64_t blk;
    int ok = 1;
    
    if (eh->eh_depth == 0) {
        /* Leaf node */
        retval = dump_leaf_extent(ctx, eh);
        if (!retval) {
            LOG_ERROR("dump_leaf_extent failed");
            return 0;
        }
    } else if (eh->eh_depth <= 4) {
        /* Index node */
        for (i = 0; i < ext2fs_le16_to_cpu(eh->eh_entries); i++) {
            retval = ext2fs_get_mem(ctx->blocksize, &buf);
            if (retval) {
                return 0;
            }
            memset(buf, 0, ctx->blocksize);
            
            ei = EXT_FIRST_INDEX(eh) + i;
            blk = ext2fs_le32_to_cpu(ei->ei_leaf) +
                 ((__u64)ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32);
            
            retval = io_channel_read_blk64(handle->fs->io, blk, 1, buf);
            if (retval) {
                ext2fs_free_mem(&buf);
                ok = 0;
                break;
            }
            
            next = (struct ext3_extent_header *)buf;
            
            /* Validate before recursing */
            if (!validate_extent_header(next, ctx->blocksize)) {
                LOG_WARN("Invalid extent header at block %llu",
                        (unsigned long long)blk);
                ext2fs_free_mem(&buf);
                continue;
            }
            
            retval = extent_tree_travel(ctx, handle, next);
            ext2fs_free_mem(&buf);
            
            if (!retval) {
                LOG_WARN("extent_tree_travel failed for child block");
            }
        }
    } else {
        LOG_ERROR("Extent tree depth > 4");
        return 1;
    }
    
    return ok;
}

/*
 * Parse inode extent tree
 */
static int parse_ino_extent(struct recover_context *ctx,
                           ext2_extent_handle_t handle)
{
    int i, retval;
    struct ext3_extent_idx *ix;
    struct ext3_extent_header *next;
    char *buf = NULL;
    blk64_t blk;
    struct ext3_extent_header *eh;
    int ok = 1;
    
    eh = (struct ext3_extent_header *)handle->inode->i_block;
    
    retval = ext2fs_get_mem(ctx->blocksize, &buf);
    if (retval)
        return 0;
    memset(buf, 0, ctx->blocksize);
    
    /* Scan up to 4 index entries (or until eh_entries < max) */
    int max_scan = 4;
    if (ext2fs_le16_to_cpu(eh->eh_entries) < max_scan)
        max_scan = ext2fs_le16_to_cpu(eh->eh_entries);
    
    for (i = 0; i < max_scan; i++) {
        ix = EXT_FIRST_INDEX(eh) + i;
        
        blk = ext2fs_le32_to_cpu(ix->ei_leaf) +
             ((__u64)ext2fs_le16_to_cpu(ix->ei_leaf_hi) << 32);
        
        retval = io_channel_read_blk64(handle->fs->io, blk, 1, buf);
        if (retval) {
            ok = 0;
            break;
        }
        
        next = (struct ext3_extent_header *)buf;
        
        /* Validate */
        if (!validate_extent_header(next, ctx->blocksize)) {
            LOG_WARN("Invalid child extent header");
            ok = 0;
            break;
        }
        
        retval = extent_tree_travel(ctx, handle, next);
        if (!retval) {
            LOG_ERROR("extent_tree_travel failed");
            ok = 0;
            break;
        }
        
        /* Stop if this block wasn't full (no more children expected) */
        int max_entries = calculate_max_entries(ctx->blocksize, 
                                               next->eh_depth == 0);
        if (ext2fs_le16_to_cpu(next->eh_entries) < max_entries) {
            break;
        }
    }
    
    ext2fs_free_mem(&buf);
    return ok;
}

/*
 * Check if inode extent is partially intact
 */
static int is_inode_extent_clear(struct ext2_inode *inode)
{
    struct ext3_extent_header *eh;
    struct ext3_extent_idx *ei;
    blk64_t blk;
    
    eh = (struct ext3_extent_header *)inode->i_block;
    ei = (struct ext3_extent_idx *)(eh + 1);
    
    blk = ext2fs_le32_to_cpu(ei->ei_leaf) +
         ((__u64)ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32);
    
    if (ei->ei_leaf > 1 && LINUX_S_ISREG(inode->i_mode) && 
        inode->i_links_count == 0) {
        return 0; /* Not clear, can recover */
    }
    
    return 1; /* Clear, cannot recover */
}

/*
 * Recover from extent tree (traditional method)
 */
int recover_from_extent_tree(struct recover_context *ctx, __u32 ino,
                            struct ext2_inode *inode)
{
    ext2_extent_handle_t handle;
    errcode_t retval;
    
    /* Create recovery file */
    retval = create_recovery_file(ctx, ino, &ctx->recover_fd);
    if (retval < 0) {
        return -1;
    }
    
    /* Open extent handle */
    retval = ext2fs_extent_open(ctx->fs, ino, &handle);
    if (retval) {
        close(ctx->recover_fd);
        return retval;
    }
    
    /* Parse extent tree */
    retval = parse_ino_extent(ctx, handle);
    
    ext2fs_extent_free(handle);
    close(ctx->recover_fd);
    ctx->recover_fd = -1;
    
    return retval ? 0 : -1;
}

/*
 * Print recovery statistics
 */
void print_stats(struct recover_context *ctx)
{
    LOG_INFO("=== Recovery Statistics ===");
    LOG_INFO("Total files recovered: %lu", ctx->files_recovered);
    LOG_INFO("Files failed: %lu", ctx->files_failed);
    LOG_INFO("Orphan list recovered: %lu", ctx->orphan_recovered);
    LOG_INFO("Journal recovered: %lu", ctx->journal_recovered);
    LOG_INFO("Aggressive scan recovered: %lu", ctx->aggressive_recovered);
}

/*
 * Usage
 */
static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] /dev/xxx\n", progname);
    fprintf(stderr, "Enhanced ext4 file recovery tool\n");
    fprintf(stderr, "Version: %s\n\n", VERSION);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --normal       Traditional multi-level extent recovery (default)\n");
    fprintf(stderr, "  --orphan       Priority recovery from orphan list\n");
    fprintf(stderr, "  --journal      Recover from journal (for single-level extents)\n");
    fprintf(stderr, "  --aggressive   Full-disk scan for orphaned extent blocks\n");
    fprintf(stderr, "  --all          Use all recovery strategies\n");
    fprintf(stderr, "  --verbose      Enable verbose output\n");
    fprintf(stderr, "  --trim         Trim trailing zero blocks\n");
    fprintf(stderr, "  --dir <path>   Recovery directory (default: ./RECOVER)\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --all /dev/sdb1\n", progname);
    fprintf(stderr, "  %s --orphan --journal /dev/sdb1\n", progname);
}

/*
 * Main
 */
int main(int argc, char **argv)
{
    errcode_t retval;
    blk64_t use_superblock = 0;
    int use_blocksize = 0;
    int flags;
    __u32 imax;
    struct ext2_inode inode;
    int i;
    
    /* Initialize context */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.mode = RECOVER_MODE_NORMAL;
    g_ctx.journal_fd = -1;
    g_ctx.recover_fd = -1;
    strcpy(g_ctx.recover_dir, RECOVER_DIR);
    
    /* Parse arguments */
    if (argc < 2) {
        usage(argv[0]);
        exit(1);
    }
    
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--normal") == 0) {
            g_ctx.mode |= RECOVER_MODE_NORMAL;
        } else if (strcmp(argv[i], "--orphan") == 0) {
            g_ctx.mode |= RECOVER_MODE_ORPHAN;
        } else if (strcmp(argv[i], "--journal") == 0) {
            g_ctx.mode |= RECOVER_MODE_JOURNAL;
        } else if (strcmp(argv[i], "--aggressive") == 0) {
            g_ctx.mode |= RECOVER_MODE_AGGRESSIVE;
        } else if (strcmp(argv[i], "--all") == 0) {
            g_ctx.mode = RECOVER_MODE_ALL;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_ctx.verbose = 1;
        } else if (strcmp(argv[i], "--trim") == 0) {
            g_ctx.trim_zeros = 1;
        } else if (strcmp(argv[i], "--dir") == 0) {
            if (i + 1 < argc) {
                strncpy(g_ctx.recover_dir, argv[++i], sizeof(g_ctx.recover_dir) - 1);
            }
        } else if (argv[i][0] != '-') {
            g_ctx.device_name = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            exit(1);
        }
    }
    
    if (!g_ctx.device_name) {
        fprintf(stderr, "Error: No device specified\n");
        usage(argv[0]);
        exit(1);
    }
    
    LOG_INFO("ext4recover v%s starting", VERSION);
    LOG_INFO("Device: %s", g_ctx.device_name);
    LOG_INFO("Recovery mode: 0x%02x", g_ctx.mode);
    
    /* Open filesystem */
    flags = EXT2_FLAG_JOURNAL_DEV_OK | EXT2_FLAG_SOFTSUPP_FEATURES |
           EXT2_FLAG_64BITS;
    retval = ext2fs_open(g_ctx.device_name, flags, use_superblock,
                        use_blocksize, unix_io_manager, &g_ctx.fs);
    if (retval) {
        com_err("ext4recover", retval, "while opening %s", g_ctx.device_name);
        fprintf(stderr, "Couldn't find valid filesystem superblock.\n");
        exit(retval);
    }
    
    g_ctx.fs->default_bitmap_type = EXT2FS_BMAP64_RBTREE;
    g_ctx.blocksize = g_ctx.fs->blocksize;
    g_ctx.total_blocks = ext2fs_blocks_count(g_ctx.fs->super);
    
    LOG_INFO("Block size: %zd bytes", g_ctx.blocksize);
    LOG_INFO("Total blocks: %llu", (unsigned long long)g_ctx.total_blocks);
    
    /* Open device for direct reading */
    g_ctx.device_fd = open(g_ctx.device_name, O_RDONLY);
    if (g_ctx.device_fd < 0) {
        perror("open(device)");
        ext2fs_close(g_ctx.fs);
        exit(1);
    }
    
    /* Create recovery directory */
    retval = mkdir(g_ctx.recover_dir, 0750);
    if (retval < 0 && errno != EEXIST) {
        perror("mkdir");
        close(g_ctx.device_fd);
        ext2fs_close(g_ctx.fs);
        exit(1);
    }
    
    /* Safety check */
    if (is_on_device(g_ctx.recover_dir, g_ctx.device_name) == 1) {
        LOG_ERROR("DANGER: recovery dir '%s' is on target device '%s', aborted!",
                 g_ctx.recover_dir, g_ctx.device_name);
        close(g_ctx.device_fd);
        ext2fs_close(g_ctx.fs);
        exit(1);
    }
    
    /* Execute recovery strategies */
    
    /* P0-1: Orphan list recovery */
    if (g_ctx.mode & RECOVER_MODE_ORPHAN) {
        LOG_INFO("=== Phase 1: Orphan List Recovery ===");
        recover_orphan_list(&g_ctx);
    }
    
    /* P0-2: Journal recovery */
    if (g_ctx.mode & RECOVER_MODE_JOURNAL) {
        LOG_INFO("=== Phase 2: Journal Recovery ===");
        if (init_journal(&g_ctx) == 0) {
            recover_from_journal(&g_ctx);
            close_journal(&g_ctx);
        }
    }
    
    /* Traditional extent tree recovery */
    if (g_ctx.mode & RECOVER_MODE_NORMAL) {
        LOG_INFO("=== Phase 3: Traditional Extent Tree Recovery ===");
        imax = g_ctx.fs->super->s_inodes_count;
        
        for (__u32 icount = 3; icount <= imax; icount++) {
            retval = ext2fs_read_inode(g_ctx.fs, icount, &inode);
            if (retval) {
                continue;
            }
            
            if (is_inode_extent_clear(&inode)) {
                continue;
            }
            
            LOG_DEBUG(&g_ctx, "Processing inode %u", icount);
            
            retval = recover_from_extent_tree(&g_ctx, icount, &inode);
            if (retval == 0) {
                g_ctx.files_recovered++;
            } else {
                g_ctx.files_failed++;
            }
        }
    }
    
    /* P1-1: Aggressive scan */
    if (g_ctx.mode & RECOVER_MODE_AGGRESSIVE) {
        LOG_INFO("=== Phase 4: Aggressive Full-Disk Scan ===");
        aggressive_scan(&g_ctx);
    }
    
    /* Cleanup */
    close(g_ctx.device_fd);
    ext2fs_close(g_ctx.fs);
    
    /* Print statistics */
    print_stats(&g_ctx);
    
    LOG_INFO("Recovery complete!");
    return 0;
}
