/*
 * ext4recover_v5.c - Enhanced ext4 file recovery tool
 * 
 * v0.5 enhancements:
 * - Bigalloc/Cluster support
 * - Directory entry recovery (filename mapping)
 * - Orphan list improvement
 * - Checkpoint/resume capability
 */

#include "ext4_common_v5.h"

static struct recover_context g_ctx;

/*
 * Utility: Check if recovery directory is on target device
 */
int is_on_device(const char *path, const char *dev)
{
    struct stat stat1, stat2;
    int ret;
    
    ret = stat(path, &stat1);
    if (ret < 0) return -1;
    
    ret = stat(dev, &stat2);
    if (ret < 0) return -1;
    
    if (((stat2.st_mode & S_IFMT) == S_IFBLK) && 
        (stat1.st_dev == stat2.st_rdev)) {
        return 1;
    }
    
    return 0;
}

/*
 * Recover blocks to file with bigalloc support
 */
int recover_block_to_file(int devfd, int inofd, __le32 block, __le16 len,
                         __u64 start, ssize_t blocksize, 
                         struct recover_context *ctx)
{
    off_t offset_dev, offset_ino;
    ssize_t got, ret;
    char *buf;
    int i;
    
    /* Apply cluster ratio if bigalloc is enabled */
    blk64_t phys_block = cluster_to_block(ctx, start);
    blk64_t block_len = cluster_len_to_blocks(ctx, len);
    
    buf = malloc(blocksize);
    if (!buf) {
        LOG_ERROR("Failed to allocate block buffer");
        return 0;
    }
    
    offset_dev = lseek(devfd, (off_t)phys_block * blocksize, SEEK_SET);
    if (offset_dev < 0) {
        free(buf);
        return 0;
    }
    
    offset_ino = lseek(inofd, (off_t)block * blocksize, SEEK_SET);
    if (offset_ino < 0) {
        free(buf);
        return 0;
    }
    
    for (i = 0; i < block_len; i++) {
        ssize_t off = 0;
        
        while (off < blocksize) {
            got = read(devfd, buf + off, blocksize - off);
            if (got <= 0) {
                free(buf);
                return 0;
            }
            off += got;
        }
        
        ssize_t written = 0;
        while (written < blocksize) {
            ret = write(inofd, buf + written, blocksize - written);
            if (ret <= 0) {
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
 * Create recovery file with optional filename mapping
 */
int create_recovery_file(struct recover_context *ctx, __u32 ino, int *fd_out)
{
    char filename[512];
    const char *original_name = get_filename_for_inode(ctx, ino);
    
    if (original_name) {
        snprintf(filename, sizeof(filename), "%s/%s", 
                ctx->recover_dir, original_name);
    } else {
        snprintf(filename, sizeof(filename), "%s/%u_file", 
                ctx->recover_dir, ino);
    }
    
    /* Don't overwrite existing recovered files */
    struct stat existing;
    if (stat(filename, &existing) == 0 && existing.st_size > 0) {
        *fd_out = -1;
        return -2;
    }
    
    *fd_out = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
    if (*fd_out < 0) {
        LOG_ERROR("Failed to create recovery file: %s", filename);
        return -1;
    }
    
    LOG_DEBUG(ctx, "Created recovery file: %s", filename);
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
    
    int max_entries = calculate_max_entries(ctx->blocksize, 1);
    
    if (ext2fs_le16_to_cpu(eh->eh_entries) > max_entries) {
        LOG_WARN("eh_entries (%u) exceeds max (%d)",
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
        
        if (ee_len == 0 || ee_start == 0)
            continue;
        
        LOG_DEBUG(ctx, "  Extent: logical=%u, len=%u, physical=%llu",
                 ee_block, ee_len, (unsigned long long)ee_start);
        
        retval = recover_block_to_file(ctx->device_fd, ctx->recover_fd,
                                      ee_block, ee_len, ee_start,
                                      ctx->blocksize, ctx);
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
                             struct ext3_extent_header *eh, int level)
{
    struct ext3_extent_idx *ei;
    struct ext3_extent_header *next_eh;
    char *buf;
    int i, ret;
    blk64_t blk;
    
    if (level == 0) {
        return dump_leaf_extent(ctx, eh);
    }
    
    buf = malloc(ctx->blocksize);
    if (!buf) return 0;
    
    ei = EXT_FIRST_INDEX(eh);
    for (i = 0; i < ext2fs_le16_to_cpu(eh->eh_entries); i++, ei++) {
        blk = ((__u64)ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32) +
              (__u64)ext2fs_le32_to_cpu(ei->ei_leaf);
        
        /* Apply cluster ratio */
        blk = cluster_to_block(ctx, blk);
        
        if (lseek(ctx->device_fd, blk * ctx->blocksize, SEEK_SET) < 0) {
            free(buf);
            return 0;
        }
        
        if (read(ctx->device_fd, buf, ctx->blocksize) != ctx->blocksize) {
            free(buf);
            return 0;
        }
        
        next_eh = (struct ext3_extent_header *)buf;
        ret = extent_tree_travel(ctx, handle, next_eh, level - 1);
        if (!ret) {
            free(buf);
            return 0;
        }
    }
    
    free(buf);
    return 1;
}

/*
 * Recover from extent tree
 */
int recover_from_extent_tree(struct recover_context *ctx, __u32 ino,
                             struct ext2_inode *inode)
{
    ext2_extent_handle_t handle;
    struct ext3_extent_header *eh;
    int ret;
    
    ret = ext2fs_extent_open2(ctx->fs, ino, inode, &handle);
    if (ret) return 0;
    
    eh = (struct ext3_extent_header *)inode->i_block;
    int depth = ext2fs_le16_to_cpu(eh->eh_depth);
    
    ret = extent_tree_travel(ctx, handle, eh, depth);
    
    ext2fs_extent_free(handle);
    return ret;
}

/*
 * Print statistics
 */
void print_stats(struct recover_context *ctx)
{
    LOG_INFO("========== Recovery Statistics ==========");
    LOG_INFO("Total files recovered:     %lu", ctx->files_recovered);
    LOG_INFO("  - From journal:          %lu", ctx->journal_recovered);
    LOG_INFO("  - From orphan list:      %lu", ctx->orphan_recovered);
    LOG_INFO("  - From aggressive scan:  %lu", ctx->aggressive_recovered);
    LOG_INFO("Failed recoveries:         %lu", ctx->files_failed);
    LOG_INFO("Filename mappings found:   %d", ctx->filename_count);
    if (ctx->has_bigalloc) {
        LOG_INFO("Bigalloc cluster ratio:    %d", ctx->cluster_ratio);
    }
    LOG_INFO("=========================================");
}

/*
 * Usage
 */
static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options] <device>\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --journal       Recover from journal (default)\n");
    fprintf(stderr, "  --orphan        Recover from orphan list\n");
    fprintf(stderr, "  --aggressive    Full-disk aggressive scan\n");
    fprintf(stderr, "  --all           Use all recovery methods\n");
    fprintf(stderr, "  --resume        Resume from checkpoint\n");
    fprintf(stderr, "  --dir <path>    Recovery directory (default: ./RECOVER)\n");
    fprintf(stderr, "  --verbose       Enable verbose output\n");
    fprintf(stderr, "  --version       Show version\n");
    fprintf(stderr, "\nVersion: %s\n", VERSION);
}

/*
 * Main
 */
int main(int argc, char *argv[])
{
    errcode_t retval;
    int i, ret;
    
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.mode = RECOVER_MODE_JOURNAL;
    strcpy(g_ctx.recover_dir, RECOVER_DIR);
    
    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--journal") == 0) {
            g_ctx.mode = RECOVER_MODE_JOURNAL;
        } else if (strcmp(argv[i], "--orphan") == 0) {
            g_ctx.mode = RECOVER_MODE_ORPHAN;
        } else if (strcmp(argv[i], "--aggressive") == 0) {
            g_ctx.mode = RECOVER_MODE_AGGRESSIVE;
        } else if (strcmp(argv[i], "--all") == 0) {
            g_ctx.mode = RECOVER_MODE_ALL;
        } else if (strcmp(argv[i], "--resume") == 0) {
            g_ctx.resume_mode = 1;
        } else if (strcmp(argv[i], "--dir") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 1;
            }
            strncpy(g_ctx.recover_dir, argv[i], sizeof(g_ctx.recover_dir)-1);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_ctx.verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("ext4recover version %s\n", VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            g_ctx.device_name = argv[i];
        }
    }
    
    if (!g_ctx.device_name) {
        usage(argv[0]);
        return 1;
    }
    
    LOG_INFO("ext4recover v%s starting...", VERSION);
    
    /* Setup checkpoint file path */
    snprintf(g_ctx.checkpoint_file, sizeof(g_ctx.checkpoint_file),
             "%s/%s", g_ctx.recover_dir, CHECKPOINT_FILE);
    
    /* Load checkpoint if resume mode */
    if (g_ctx.resume_mode) {
        if (load_checkpoint(&g_ctx) == 0) {
            LOG_INFO("Resuming from checkpoint...");
        } else {
            LOG_WARN("Resume requested but no valid checkpoint found");
            g_ctx.resume_mode = 0;
        }
    }
    
    /* Check recovery directory */
    if (is_on_device(g_ctx.recover_dir, g_ctx.device_name) == 1) {
        LOG_ERROR("Recovery directory is on target device!");
        return 1;
    }
    
    /* Create recovery directory */
    mkdir(g_ctx.recover_dir, 0755);
    
    /* Open filesystem */
    fprintf(stderr, "[DEBUG] About to call ext2fs_open for: %s
", g_ctx.device_name);
    retval = ext2fs_open(g_ctx.device_name, 0, 0, 0,
                        unix_io_manager, &g_ctx.fs);
    fprintf(stderr, "[DEBUG] ext2fs_open returned: %lu (0x%lx)
", (unsigned long)retval, (unsigned long)retval);
    if (retval) {
        LOG_ERROR("Failed to open filesystem: %d", retval);
        return 1;
    }
    
    g_ctx.blocksize = g_ctx.fs->blocksize;
    g_ctx.total_blocks = ext2fs_blocks_count(g_ctx.fs->super);
    
    /* Initialize cluster/bigalloc info */
    init_cluster_info(&g_ctx);
    
    /* Open device for raw access */
    g_ctx.device_fd = open(g_ctx.device_name, O_RDONLY | O_LARGEFILE);
    if (g_ctx.device_fd < 0) {
        LOG_ERROR("Failed to open device for reading");
        ext2fs_close(g_ctx.fs);
        return 1;
    }
    
    /* Initialize filename mapping */
    if (init_filename_map(&g_ctx) != 0) {
        LOG_WARN("Failed to initialize filename map");
    } else {
        parse_directory_blocks(&g_ctx);
    }
    
    /* Initialize journal if needed */
    if (g_ctx.mode & (RECOVER_MODE_JOURNAL | RECOVER_MODE_ALL)) {
        if (init_journal(&g_ctx) == 0) {
            LOG_INFO("Journal initialized successfully");
        }
    }
    
    /* Phase 1: Orphan list */
    if (g_ctx.mode & (RECOVER_MODE_ORPHAN | RECOVER_MODE_ALL)) {
        LOG_INFO("Phase 1: Orphan list recovery...");
        ret = recover_orphan_list(&g_ctx);
        if (ret > 0) {
            LOG_INFO("Orphan recovery completed: %d files", ret);
        }
        
        /* Save checkpoint */
        if (g_ctx.files_recovered % 100 == 0) {
            save_checkpoint(&g_ctx);
        }
    }
    
    /* Phase 2: Journal parsing */
    if (g_ctx.mode & (RECOVER_MODE_JOURNAL | RECOVER_MODE_ALL)) {
        LOG_INFO("Phase 2: Journal recovery...");
        ret = recover_from_journal(&g_ctx);
        if (ret > 0) {
            LOG_INFO("Journal recovery completed: %d files", ret);
        }
        
        /* Save checkpoint */
        save_checkpoint(&g_ctx);
    }
    
    /* Phase 3: Aggressive scan */
    if (g_ctx.mode & (RECOVER_MODE_AGGRESSIVE | RECOVER_MODE_ALL)) {
        LOG_INFO("Phase 3: Aggressive full-disk scan...");
        ret = aggressive_scan(&g_ctx);
        if (ret > 0) {
            LOG_INFO("Aggressive scan completed: %d files", ret);
        }
        
        /* Save checkpoint */
        save_checkpoint(&g_ctx);
    }
    
    /* Final statistics */
    print_stats(&g_ctx);
    
    /* Clear checkpoint on successful completion */
    clear_checkpoint(&g_ctx);
    
    /* Cleanup */
    free_filename_map(&g_ctx);
    if (g_ctx.has_journal) {
        close_journal(&g_ctx);
    }
    close(g_ctx.device_fd);
    ext2fs_close(g_ctx.fs);
    
    LOG_INFO("Recovery completed. Files saved to: %s", g_ctx.recover_dir);
    return 0;
}
