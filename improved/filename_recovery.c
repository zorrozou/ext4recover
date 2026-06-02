/*
 * filename_recovery.c - Directory entry recovery for ext4recover
 */

#include "ext4_common_v5.h"

/*
 * Initialize filename mapping structure
 */
int init_filename_map(struct recover_context *ctx)
{
    ctx->filename_capacity = MAX_FILENAME_MAP;
    ctx->filename_map = calloc(ctx->filename_capacity, 
                               sizeof(struct filename_entry));
    if (!ctx->filename_map) {
        LOG_ERROR("Failed to allocate filename map");
        return -1;
    }
    ctx->filename_count = 0;
    return 0;
}

/*
 * Add inode->filename mapping
 */
int add_filename_mapping(struct recover_context *ctx, __u32 inode, 
                        const char *name)
{
    if (ctx->filename_count >= ctx->filename_capacity) {
        LOG_WARN("Filename map full, cannot add more entries");
        return -1;
    }
    
    /* Check if already exists */
    for (int i = 0; i < ctx->filename_count; i++) {
        if (ctx->filename_map[i].inode == inode) {
            /* Update existing entry */
            strncpy(ctx->filename_map[i].name, name, 255);
            ctx->filename_map[i].name[255] = '\0';
            return 0;
        }
    }
    
    /* Add new entry */
    ctx->filename_map[ctx->filename_count].inode = inode;
    strncpy(ctx->filename_map[ctx->filename_count].name, name, 255);
    ctx->filename_map[ctx->filename_count].name[255] = '\0';
    ctx->filename_count++;
    
    LOG_DEBUG(ctx, "Mapped inode %u -> '%s'", inode, name);
    return 0;
}

/*
 * Get filename for inode
 */
const char* get_filename_for_inode(struct recover_context *ctx, __u32 inode)
{
    for (int i = 0; i < ctx->filename_count; i++) {
        if (ctx->filename_map[i].inode == inode) {
            return ctx->filename_map[i].name;
        }
    }
    return NULL;
}

/*
 * Parse a directory block and extract filename mappings
 */
static void parse_dir_block(struct recover_context *ctx, char *block_data)
{
    struct ext2_dir_entry_2 *dirent;
    unsigned int offset = 0;
    
    while (offset < ctx->blocksize) {
        dirent = (struct ext2_dir_entry_2 *)(block_data + offset);
        
        /* Sanity checks */
        if (dirent->rec_len == 0 || dirent->rec_len > ctx->blocksize - offset)
            break;
        
        if (dirent->inode != 0 && dirent->name_len > 0 && 
            dirent->name_len < 256) {
            char name[256];
            memcpy(name, dirent->name, dirent->name_len);
            name[dirent->name_len] = '\0';
            
            /* Skip '.' and '..' */
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                add_filename_mapping(ctx, dirent->inode, name);
            }
        }
        
        offset += dirent->rec_len;
    }
}

/*
 * Scan journal for directory blocks
 */
void parse_directory_blocks(struct recover_context *ctx)
{
    if (!ctx->has_journal) {
        LOG_DEBUG(ctx, "No journal, skipping directory block parsing");
        return;
    }
    
    LOG_INFO("Parsing directory blocks from journal...");
    
    char *block_buf = malloc(ctx->blocksize);
    if (!block_buf) {
        LOG_ERROR("Failed to allocate block buffer");
        return;
    }
    
    /* Scan journal blocks */
    for (blk64_t blk = ctx->journal_start; 
         blk < ctx->journal_start + ctx->journal_len; blk++) {
        
        off_t offset = lseek(ctx->device_fd, blk * ctx->blocksize, SEEK_SET);
        if (offset < 0)
            continue;
        
        ssize_t got = read(ctx->device_fd, block_buf, ctx->blocksize);
        if (got != ctx->blocksize)
            continue;
        
        /* Check if this looks like a directory block */
        struct ext2_dir_entry_2 *first = (struct ext2_dir_entry_2 *)block_buf;
        if (first->inode != 0 && first->rec_len > 0 && 
            first->rec_len <= ctx->blocksize) {
            /* Likely a directory block */
            parse_dir_block(ctx, block_buf);
        }
    }
    
    free(block_buf);
    LOG_INFO("Found %d filename mappings", ctx->filename_count);
}

/*
 * Free filename map
 */
void free_filename_map(struct recover_context *ctx)
{
    if (ctx->filename_map) {
        free(ctx->filename_map);
        ctx->filename_map = NULL;
    }
    ctx->filename_count = 0;
}
