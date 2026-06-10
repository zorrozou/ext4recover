/*
 * orphan_recovery.c - Orphan list recovery module
 * 
 * Recovers files from the ext4 orphan inode list.
 * Orphan inodes are those that were unlinked but their truncate
 * operation was not completed (e.g., system crash during deletion).
 * These have the highest probability of successful recovery.
 */

#include "ext4_common_v5.h"

/*
 * Scan the orphan inode chain starting from start_ino
 * The orphan list is a singly-linked list stored in i_dtime field
 */
int scan_orphan_chain(struct recover_context *ctx, __u32 start_ino)
{
    __u32 current_ino = start_ino;
    struct ext2_inode inode;
    errcode_t retval;
    int count = 0;
    
    LOG_INFO("Scanning orphan chain starting from inode %u", start_ino);
    
    while (current_ino != 0) {
        /* Prevent infinite loops from corrupted chains */
        if (count > 10000) {
            LOG_WARN("Orphan chain too long (>10000), possible corruption");
            break;
        }
        
        LOG_DEBUG(ctx, "Processing orphan inode %u", current_ino);
        
        /* Read the orphan inode */
        retval = ext2fs_read_inode(ctx->fs, current_ino, &inode);
        if (retval) {
            LOG_ERROR("Failed to read orphan inode %u", current_ino);
            break;
        }

        /* A4: i_dtime is the next-orphan pointer ONLY while an inode is
         * on the orphan list; on ordinary deleted inodes it is a
         * deletion timestamp. Validate like kernel ext4_orphan_get(). */
        __u32 next_ino = inode.i_dtime;
        if (next_ino > ctx->fs->super->s_inodes_count)
            next_ino = 0;   /* timestamp, not a chain pointer */

        /* Validate this is actually a deleted file */
        if (!LINUX_S_ISREG(inode.i_mode) || inode.i_links_count != 0) {
            LOG_DEBUG(ctx, "Inode %u not a deleted regular file, skipping",
                     current_ino);
            current_ino = next_ino; /* Next in chain */
            count++;
            continue;
        }

        /* Check if it has extent flag */
        if (!(inode.i_flags & EXT4_EXTENTS_FL)) {
            LOG_DEBUG(ctx, "Inode %u does not use extents, skipping",
                     current_ino);
            current_ino = next_ino;
            count++;
            continue;
        }
        
        LOG_INFO("Found orphan file inode %u, attempting recovery", current_ino);
        
        /* Try to recover this inode */
        retval = recover_from_extent_tree(ctx, current_ino, &inode);
        if (retval == 0) {
            ctx->orphan_recovered++;
            ctx->files_recovered++;
            LOG_INFO("Successfully recovered orphan inode %u", current_ino);
        } else {
            ctx->files_failed++;
            LOG_WARN("Failed to recover orphan inode %u", current_ino);
        }
        
        /* Move to next orphan in chain (validated above) */
        current_ino = next_ino;
        count++;
    }
    
    LOG_INFO("Scanned %d orphan inodes", count);
    return 0;
}

/*
 * Recover files from the orphan list
 * The orphan list head is stored in s_last_orphan field of superblock
 */
int recover_orphan_list(struct recover_context *ctx)
{
    __u32 orphan_head;
    
    if (!ctx || !ctx->fs || !ctx->fs->super) {
        LOG_ERROR("Invalid context or filesystem");
        return -1;
    }
    
    /* Get the orphan list head from superblock */
    orphan_head = ctx->fs->super->s_last_orphan;
    
    if (orphan_head == 0) {
        LOG_INFO("No orphan inodes found in superblock");
        return 0;
    }
    
    LOG_INFO("Orphan list head: inode %u", orphan_head);
    LOG_INFO("Starting orphan list recovery...");
    
    /* Scan the orphan chain */
    return scan_orphan_chain(ctx, orphan_head);
}

/*
 * Check if an inode is in orphan state (for non-chain scanning)
 * This can be used during full inode scan to prioritize orphans
 */
int is_orphan_inode(struct ext2_inode *inode)
{
    /* Orphan inode characteristics:
     * - Regular file
     * - i_links_count == 0 (unlinked)
     * - i_dtime != 0 (used for orphan chain or deletion time)
     * - Still has blocks allocated
     */
    if (!LINUX_S_ISREG(inode->i_mode))
        return 0;
        
    if (inode->i_links_count != 0)
        return 0;
        
    if (inode->i_blocks == 0)
        return 0;
        
    return 1;
}
