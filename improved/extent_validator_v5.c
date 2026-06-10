/*
 * extent_validator.c - Extent tree validation and utilities
 * 
 * Provides validation and manipulation functions for extent structures.
 * Includes dynamic eh_max calculation and deduplication.
 */

#include "ext4_common_v5.h"

/*
 * Calculate maximum entries for extent header based on block size
 * 
 * For leaf blocks:
 *   max = (blocksize - sizeof(ext4_extent_header)) / sizeof(ext4_extent)
 * For index blocks:
 *   max = (blocksize - sizeof(ext4_extent_header)) / sizeof(ext4_extent_idx)
 */
int calculate_max_entries(int blocksize, int is_leaf)
{
    int available_space = blocksize - sizeof(struct ext3_extent_header);
    int entry_size;
    
    if (is_leaf) {
        /* Also account for possible extent tail */
        entry_size = sizeof(struct ext3_extent);
    } else {
        entry_size = sizeof(struct ext3_extent_idx);
    }
    
    return available_space / entry_size;
}

/*
 * Validate extent header structure
 * Returns 1 if valid, 0 if invalid
 */
int validate_extent_header(struct ext3_extent_header *eh, int blocksize)
{
    int max_possible;
    
    if (!eh)
        return 0;
    
    /* Check magic number */
    if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT4_EXT_MAGIC)
        return 0;
    
    /* Check depth (ext4 supports up to 5 levels). Use le16 decode
     * to be byte-order safe (eh_depth is __le16). */
    if (ext2fs_le16_to_cpu(eh->eh_depth) > 5)
        return 0;
    
    /* Calculate maximum possible entries based on block size */
    max_possible = calculate_max_entries(blocksize, eh->eh_depth == 0);
    
    /* Check eh_max is reasonable */
    if (ext2fs_le16_to_cpu(eh->eh_max) > max_possible)
        return 0;
    
    /* Check entries doesn't exceed max */
    if (ext2fs_le16_to_cpu(eh->eh_entries) > ext2fs_le16_to_cpu(eh->eh_max))
        return 0;
    
    return 1;
}

/*
 * Compare two extents for deduplication
 * Returns 1 if identical, 0 otherwise
 */
static int extents_equal(struct ext3_extent *e1, struct ext3_extent *e2)
{
    return (e1->ee_block == e2->ee_block &&
            e1->ee_len == e2->ee_len &&
            e1->ee_start == e2->ee_start &&
            e1->ee_start_hi == e2->ee_start_hi);
}

/*
 * Deduplicate extent entries
 * This handles the case where memmove left duplicate entries in the array
 * Returns the new count of unique extents
 */
int deduplicate_extents(struct ext3_extent *extents, int count)
{
    int i, j, unique_count;
    
    if (count <= 1)
        return count;
    
    unique_count = 1; /* First extent is always unique */
    
    /* For each extent after the first */
    for (i = 1; i < count; i++) {
        int is_duplicate = 0;
        
        /* Check if it's a duplicate of any previous unique extent */
        for (j = 0; j < unique_count; j++) {
            if (extents_equal(&extents[i], &extents[j])) {
                is_duplicate = 1;
                break;
            }
        }
        
        /* If not a duplicate, add it to the unique set */
        if (!is_duplicate) {
            if (unique_count != i) {
                /* Move it to the unique section */
                memcpy(&extents[unique_count], &extents[i], 
                      sizeof(struct ext3_extent));
            }
            unique_count++;
        }
    }
    
    return unique_count;
}


/*
 * Validate extent index entry
 */
int validate_extent_idx(struct ext3_extent_idx *ei, blk64_t device_blocks)
{
    blk64_t leaf;
    
    if (!ei)
        return 0;
    
    leaf = ext2fs_le32_to_cpu(ei->ei_leaf) +
           ((__u64)ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32);
    
    /* Check if block number is valid */
    if (leaf == 0 || leaf >= device_blocks)
        return 0;
    
    return 1;
}

