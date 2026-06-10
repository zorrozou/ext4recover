/*
 * utils_v5.c - Utility functions for ext4recover v0.5
 * 
 * Contains:
 * - Bigalloc/cluster support functions
 * - Filename mapping (directory entry recovery)
 * - Checkpoint/resume functionality
 */

#include "ext4_common_v5.h"
#include <dirent.h>

/* ============================================================
 * BIGALLOC / CLUSTER SUPPORT
 * ============================================================ */

/*
 * Initialize cluster info from filesystem superblock
 */
void init_cluster_info(struct recover_context *ctx)
{
    struct ext2_super_block *sb = ctx->fs->super;
    
    ctx->log_cluster_size = sb->s_log_cluster_size;
    
    /* bigalloc: cluster_size > block_size */
    if (sb->s_log_cluster_size > sb->s_log_block_size) {
        ctx->has_bigalloc = 1;
        ctx->cluster_ratio = 1 << (sb->s_log_cluster_size - sb->s_log_block_size);
        LOG_INFO("Bigalloc detected: cluster_ratio=%d (cluster_size=%d, block_size=%d)",
                ctx->cluster_ratio,
                1024 << sb->s_log_cluster_size,
                ctx->blocksize);
    } else {
        ctx->has_bigalloc = 0;
        ctx->cluster_ratio = 1;
    }
}



/* ============================================================
 * FILENAME MAPPING (DIRECTORY ENTRY RECOVERY)
 * ============================================================ */

/*
 * B4: ino -> filename_map index hash (open addressing, linear probe).
 * add_filename_mapping was O(N) dup-check per insert (O(N^2) build,
 * up to 1e10 compares at the 100K-entry cap) and get_filename_for_inode
 * was O(N) per recovered file. Key 0 = empty slot (inode 0 is invalid).
 */
static __u32 *fm_hkeys = NULL;
static int   *fm_hvals = NULL;
static int    fm_hsize = 0;

static void fm_hash_put(__u32 ino, int idx)
{
    if (!fm_hsize) return;
    __u32 h = (ino * 2654435761u) & (fm_hsize - 1);
    while (fm_hkeys[h] != 0 && fm_hkeys[h] != ino)
        h = (h + 1) & (fm_hsize - 1);
    fm_hkeys[h] = ino;
    fm_hvals[h] = idx;
}

static int fm_hash_get(__u32 ino)
{
    if (!fm_hsize) return -1;
    __u32 h = (ino * 2654435761u) & (fm_hsize - 1);
    while (fm_hkeys[h] != 0) {
        if (fm_hkeys[h] == ino)
            return fm_hvals[h];
        h = (h + 1) & (fm_hsize - 1);
    }
    return -1;
}

static int fm_hash_rebuild(struct recover_context *ctx, int min_size)
{
    int ns = 1024;
    while (ns < min_size * 2) ns <<= 1;   /* keep load factor <= 50% */
    __u32 *nk = calloc(ns, sizeof(__u32));
    int   *nv = calloc(ns, sizeof(int));
    if (!nk || !nv) { free(nk); free(nv); return -1; }
    free(fm_hkeys); free(fm_hvals);
    fm_hkeys = nk; fm_hvals = nv; fm_hsize = ns;
    for (int i = 0; i < ctx->filename_count; i++)
        fm_hash_put(ctx->filename_map[i].inode, i);
    return 0;
}

/*
 * Initialize filename mapping table
 */
int init_filename_map(struct recover_context *ctx)
{
    ctx->filename_capacity = 4096;
    ctx->filename_count = 0;
    ctx->filename_map = calloc(ctx->filename_capacity, sizeof(struct filename_entry));
    if (!ctx->filename_map) {
        LOG_ERROR("Failed to allocate filename map");
        return -1;
    }
    fm_hash_rebuild(ctx, ctx->filename_capacity);
    return 0;
}

/*
 * A3: sanitize a directory-entry name before it can reach a path
 * concatenation. Journal dir-block parsing is heuristic, so names may
 * come from corrupted or hostile data: a '/' or ".." would escape the
 * recovery directory entirely (path traversal).
 * Returns 1 if the name is safe to use, 0 to reject.
 */
static int filename_is_safe(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/')
            return 0;
    }
    return 1;
}

/*
 * Add a filename mapping
 */
int add_filename_mapping(struct recover_context *ctx, __u32 inode, const char *name)
{
    if (!ctx->filename_map)
        return -1;

    /* A3: never let unsafe names enter the map */
    if (!filename_is_safe(name))
        return -1;

    /* B4: O(1) duplicate check (update if exists) */
    int hidx = fm_hash_get(inode);
    if (hidx >= 0) {
        strncpy(ctx->filename_map[hidx].name, name, 255);
        ctx->filename_map[hidx].name[255] = '\0';
        return 0;
    }

    /* Grow if needed */
    if (ctx->filename_count >= ctx->filename_capacity) {
        int new_cap = ctx->filename_capacity * 2;
        if (new_cap > MAX_FILENAME_MAP) new_cap = MAX_FILENAME_MAP;
        if (ctx->filename_count >= new_cap) {
            LOG_WARN("Filename map full (%d entries)", ctx->filename_count);
            return -1;
        }
        struct filename_entry *new_map = realloc(ctx->filename_map,
                                                  new_cap * sizeof(struct filename_entry));
        if (!new_map) return -1;
        ctx->filename_map = new_map;
        ctx->filename_capacity = new_cap;
    }

    /* keep hash at <=50% load */
    if (fm_hsize && ctx->filename_count * 2 >= fm_hsize)
        fm_hash_rebuild(ctx, ctx->filename_count + 1);

    ctx->filename_map[ctx->filename_count].inode = inode;
    strncpy(ctx->filename_map[ctx->filename_count].name, name, 255);
    ctx->filename_map[ctx->filename_count].name[255] = '\0';
    fm_hash_put(inode, ctx->filename_count);
    ctx->filename_count++;

    return 0;
}

/*
 * Get filename for an inode number
 */
const char* get_filename_for_inode(struct recover_context *ctx, __u32 inode)
{
    if (!ctx->filename_map)
        return NULL;

    int idx = fm_hash_get(inode);
    if (idx >= 0 && idx < ctx->filename_count)
        return ctx->filename_map[idx].name;
    return NULL;
}

/*
 * Free filename mapping table
 */
void free_filename_map(struct recover_context *ctx)
{
    if (ctx->filename_map) {
        free(ctx->filename_map);
        ctx->filename_map = NULL;
    }
    ctx->filename_count = 0;
    ctx->filename_capacity = 0;
    free(fm_hkeys); free(fm_hvals);
    fm_hkeys = NULL; fm_hvals = NULL; fm_hsize = 0;
    free(ctx->name_claims);
    ctx->name_claims = NULL;
    ctx->claim_count = 0;
    ctx->claim_capacity = 0;
}

/*
 * A4: resolve the final output basename for an inode.
 *
 * Two different inodes can map to the same basename (same filename in
 * different directories). Previously the second one was silently
 * skipped (create path) or overwrote the first (journal rename path).
 * Now: the first inode to claim a basename owns it; later inodes are
 * diverted to "<ino>_file". Idempotent: the same inode always
 * resolves to the same answer within a run.
 */
const char *resolve_output_name(struct recover_context *ctx, __u32 ino,
                                char *buf, size_t bufsize)
{
    const char *name = get_filename_for_inode(ctx, ino);

    if (name) {
        /* existing claim? */
        int i;
        for (i = 0; i < ctx->claim_count; i++) {
            if (strncmp(ctx->name_claims[i].name, name, 255) == 0) {
                if (ctx->name_claims[i].ino == ino) {
                    snprintf(buf, bufsize, "%s", name);
                    return buf;     /* we own it */
                }
                name = NULL;        /* owned by another inode: divert */
                break;
            }
        }
        if (name && i == ctx->claim_count) {
            /* unclaimed: claim it now */
            if (ctx->claim_count >= ctx->claim_capacity) {
                int ncap = ctx->claim_capacity ? ctx->claim_capacity * 2 : 256;
                struct name_claim *nc = realloc(ctx->name_claims,
                                                ncap * sizeof(struct name_claim));
                if (nc) {
                    ctx->name_claims = nc;
                    ctx->claim_capacity = ncap;
                }
            }
            if (ctx->claim_count < ctx->claim_capacity) {
                ctx->name_claims[ctx->claim_count].ino = ino;
                snprintf(ctx->name_claims[ctx->claim_count].name,
                         sizeof(ctx->name_claims[ctx->claim_count].name),
                         "%s", name);
                ctx->claim_count++;
            }
            snprintf(buf, bufsize, "%s", name);
            return buf;
        }
    }

    snprintf(buf, bufsize, "%u_file", ino);
    return buf;
}

/*
 * Callback for directory block iteration
 */
static int dir_block_callback(ext2_filsys fs, blk64_t *blocknr,
                               e2_blkcnt_t blockcnt __attribute__((unused)),
                               blk64_t ref_blk __attribute__((unused)),
                               int ref_offset __attribute__((unused)),
                               void *priv)
{
    struct recover_context *ctx = (struct recover_context *)priv;
    char *buf;
    errcode_t retval;
    
    retval = ext2fs_get_mem(fs->blocksize, &buf);
    if (retval) return 0;
    
    retval = io_channel_read_blk64(fs->io, *blocknr, 1, buf);
    if (retval) {
        ext2fs_free_mem(&buf);
        return 0;
    }
    
    /* Parse directory entries */
    int offset = 0;
    while (offset < fs->blocksize) {
        struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)(buf + offset);
        
        /* Validate entry */
        if (de->rec_len == 0) break;
        if (offset + de->rec_len > fs->blocksize) break;
        
        if (de->inode != 0 && de->name_len > 0) {
            char name[256];
            int name_len = de->name_len;
            if (name_len > 255) name_len = 255;
            memcpy(name, de->name, name_len);
            name[name_len] = '\0';
            
            /* Skip . and .. */
            if (!(name_len == 1 && name[0] == '.') &&
                !(name_len == 2 && name[0] == '.' && name[1] == '.')) {
                add_filename_mapping(ctx, de->inode, name);
            }
        }
        
        offset += de->rec_len;
    }
    
    ext2fs_free_mem(&buf);
    return 0;
}

/*
 * Parse all directory blocks to build filename mapping
 * This scans existing (allocated) directories on the filesystem.
 * Also scans the journal for old directory blocks to find names
 * of deleted files.
 */
void parse_directory_blocks(struct recover_context *ctx)
{
    ext2_inode_scan scan;
    ext2_ino_t ino;
    struct ext2_inode inode;
    errcode_t retval;
    
    LOG_INFO("Building filename map from directory entries...");
    
    /* Method 1: Scan active directories */
    retval = ext2fs_open_inode_scan(ctx->fs, 0, &scan);
    if (retval) {
        LOG_WARN("Failed to open inode scan for directory parsing");
        return;
    }
    
    while (1) {
        retval = ext2fs_get_next_inode(scan, &ino, &inode);
        if (retval || ino == 0) break;
        
        if (!LINUX_S_ISDIR(inode.i_mode)) continue;
        if (inode.i_links_count == 0) continue;
        
        /* Iterate blocks of this directory */
        ext2fs_block_iterate3(ctx->fs, ino, BLOCK_FLAG_DATA_ONLY, 
                             NULL, dir_block_callback, ctx);
    }
    
    ext2fs_close_inode_scan(scan);
    
    /* Method 2: Scan journal for old directory blocks */
    /* Parse journal looking for directory data blocks that contain
     * entries for now-deleted inodes */
    if (ctx->has_journal) {
        LOG_INFO("Scanning journal for deleted directory entries...");
        parse_journal_directory_blocks(ctx);
    }
    
    LOG_INFO("Filename map: %d entries found", ctx->filename_count);
}

/*
 * Scan journal for directory blocks containing deleted file names
 */
void parse_journal_directory_blocks(struct recover_context *ctx)
{
    char *buf;
    errcode_t retval;
    ext2_file_t jfile = NULL;
    
    if (!ctx->has_journal || ctx->journal_len == 0)
        return;
    
    retval = ext2fs_get_mem(ctx->blocksize, &buf);
    if (retval) return;
    
    /* Open journal file ONCE for all reads */
    retval = ext2fs_file_open(ctx->fs, ctx->fs->super->s_journal_inum, 0, &jfile);
    if (retval) {
        ext2fs_free_mem(&buf);
        return;
    }
    
    for (blk64_t blk = ctx->journal_start; blk < ctx->journal_len; blk++) {
        unsigned int got;
        
        retval = ext2fs_file_llseek(jfile, (__u64)blk * ctx->blocksize, 
                                    EXT2_SEEK_SET, NULL);
        if (retval) continue;
        
        retval = ext2fs_file_read(jfile, buf, ctx->blocksize, &got);
        if (retval || (int)got != ctx->blocksize)
            continue;
        
        /* Quick heuristic: check if this looks like a directory block */
        struct ext2_dir_entry_2 *de = (struct ext2_dir_entry_2 *)buf;
        if (de->rec_len < 12 || de->rec_len > ctx->blocksize)
            continue;
        if (de->name_len == 0)
            continue;
        if (de->inode == 0 && de->rec_len == ctx->blocksize)
            continue;
        
        int looks_like_dir = 0;
        if (de->rec_len >= 12 && (de->rec_len % 4 == 0) &&
            de->name_len > 0 && de->name_len <= 255 &&
            (de->name_len + 8) <= de->rec_len) {
            if ((de->name_len == 1 && de->name[0] == '.') ||
                (de->inode > 0 && de->inode <= ctx->fs->super->s_inodes_count)) {
                looks_like_dir = 1;
            }
        }
        
        if (looks_like_dir) {
            int offset = 0;
            while (offset < ctx->blocksize) {
                de = (struct ext2_dir_entry_2 *)(buf + offset);
                if (de->rec_len == 0) break;
                if (offset + de->rec_len > ctx->blocksize) break;
                
                if (de->inode != 0 && de->name_len > 0 && de->name_len <= 255 &&
                    de->inode <= ctx->fs->super->s_inodes_count) {
                    char name[256];
                    memcpy(name, de->name, de->name_len);
                    name[de->name_len] = '\0';
                    
                    if (!(de->name_len == 1 && name[0] == '.') &&
                        !(de->name_len == 2 && name[0] == '.' && name[1] == '.')) {
                        add_filename_mapping(ctx, de->inode, name);
                    }
                }
                offset += de->rec_len;
            }
        }
    }
    
    ext2fs_file_close(jfile);
    ext2fs_free_mem(&buf);
}

/* ============================================================
 * CHECKPOINT / RESUME
 * ============================================================ */

/*
 * Save checkpoint to file
 */
int save_checkpoint(struct recover_context *ctx)
{
    FILE *fp;
    
    fp = fopen(ctx->checkpoint_file, "w");
    if (!fp) {
        LOG_DEBUG(ctx, "Failed to save checkpoint");
        return -1;
    }
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"version\": \"%s\",\n", VERSION);
    fprintf(fp, "  \"device\": \"%s\",\n", ctx->device_name);
    fprintf(fp, "  \"mode\": %d,\n", ctx->mode);
    fprintf(fp, "  \"files_recovered\": %lu,\n", ctx->files_recovered);
    fprintf(fp, "  \"journal_recovered\": %lu,\n", ctx->journal_recovered);
    fprintf(fp, "  \"orphan_recovered\": %lu,\n", ctx->orphan_recovered);
    fprintf(fp, "  \"aggressive_recovered\": %lu,\n", ctx->aggressive_recovered);
    fprintf(fp, "  \"journal_offset\": %llu,\n", 
            (unsigned long long)ctx->checkpoint_journal_offset);
    fprintf(fp, "  \"total_blocks\": %llu\n",
            (unsigned long long)ctx->total_blocks);
    fprintf(fp, "}\n");
    
    fclose(fp);
    LOG_DEBUG(ctx, "Checkpoint saved: %lu files recovered so far", ctx->files_recovered);
    return 0;
}

/*
 * Load checkpoint from file
 */
int load_checkpoint(struct recover_context *ctx)
{
    FILE *fp;
    char line[512];
    
    fp = fopen(ctx->checkpoint_file, "r");
    if (!fp) {
        return -1;
    }
    
    /* Simple JSON parser - just extract key values */
    while (fgets(line, sizeof(line), fp)) {
        unsigned long val;
        unsigned long long val64;
        char str_val[256];
        
        if (sscanf(line, "  \"files_recovered\": %lu", &val) == 1)
            ctx->checkpoint_files_recovered = val;
        else if (sscanf(line, "  \"journal_recovered\": %lu", &val) == 1)
            ctx->journal_recovered = val;
        else if (sscanf(line, "  \"orphan_recovered\": %lu", &val) == 1)
            ctx->orphan_recovered = val;
        else if (sscanf(line, "  \"aggressive_recovered\": %lu", &val) == 1)
            ctx->aggressive_recovered = val;
        else if (sscanf(line, "  \"journal_offset\": %llu", &val64) == 1)
            ctx->checkpoint_journal_offset = val64;
        else if (sscanf(line, "  \"device\": \"%255[^\"]\"", str_val) == 1) {
            /* Verify device matches */
            if (ctx->device_name && strcmp(str_val, ctx->device_name) != 0) {
                LOG_WARN("Checkpoint device '%s' doesn't match current '%s'",
                        str_val, ctx->device_name);
                fclose(fp);
                return -1;
            }
        }
    }
    
    fclose(fp);
    
    ctx->files_recovered = ctx->checkpoint_files_recovered;
    LOG_INFO("Loaded checkpoint: %lu files already recovered, journal offset=%llu",
            ctx->checkpoint_files_recovered,
            (unsigned long long)ctx->checkpoint_journal_offset);
    
    return 0;
}

/*
 * Clear checkpoint file (called on successful completion)
 */
void clear_checkpoint(struct recover_context *ctx)
{
    if (access(ctx->checkpoint_file, F_OK) == 0) {
        unlink(ctx->checkpoint_file);
        LOG_DEBUG(ctx, "Checkpoint cleared");
    }
}
