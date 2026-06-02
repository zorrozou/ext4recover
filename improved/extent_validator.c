/*
 * extent_validator.c - Extent tree validation and utilities
 * 
 * Provides validation and manipulation functions for extent structures.
 * Includes dynamic eh_max calculation and deduplication.
 */

#include "ext4_common.h"

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
    
    /* Check depth (ext4 supports up to 5 levels) */
    if (eh->eh_depth > 5)
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
 * Validate and clean extent array
 * - Removes invalid extents (len=0, start=0)
 * - Deduplicates
 * - Validates physical block numbers are within device bounds
 * Returns new count
 */
int validate_and_clean_extents(struct ext3_extent *extents, int count,
                               blk64_t device_blocks)
{
    int i, clean_count = 0;
    
    /* First pass: remove obviously invalid extents */
    for (i = 0; i < count; i++) {
        __le16 len = ext2fs_le16_to_cpu(extents[i].ee_len);
        __u64 start = (((__u64)ext2fs_le16_to_cpu(extents[i].ee_start_hi) << 32) +
                      (__u64)ext2fs_le32_to_cpu(extents[i].ee_start));
        
        /* Skip zero-length or zero-start extents */
        if (len == 0 || start == 0)
            continue;
        
        /* Check if extent is within device bounds */
        if (start + len > device_blocks)
            continue;
        
        /* Keep this extent */
        if (clean_count != i) {
            memcpy(&extents[clean_count], &extents[i], 
                  sizeof(struct ext3_extent));
        }
        clean_count++;
    }
    
    /* Second pass: deduplicate */
    clean_count = deduplicate_extents(extents, clean_count);
    
    return clean_count;
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

/*
 * Recursively validate extent tree
 * Returns 1 if tree appears valid, 0 otherwise
 */
int validate_extent_tree(char *buf, int blocksize, int max_depth,
                        blk64_t device_blocks)
{
    struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
    int i;
    
    /* Validate header */
    if (!validate_extent_header(eh, blocksize))
        return 0;
    
    /* Check depth doesn't exceed maximum */
    if (eh->eh_depth > max_depth)
        return 0;
    
    if (eh->eh_depth == 0) {
        /* Leaf node - validate extents */
        struct ext3_extent *ee = EXT_FIRST_EXTENT(eh);
        
        for (i = 0; i < ext2fs_le16_to_cpu(eh->eh_entries); i++, ee++) {
            __u64 start = (((__u64)ext2fs_le16_to_cpu(ee->ee_start_hi) << 32) +
                          (__u64)ext2fs_le32_to_cpu(ee->ee_start));
            __le16 len = ext2fs_le16_to_cpu(ee->ee_len);
            
            if (start + len > device_blocks)
                return 0;
        }
    } else {
        /* Index node - validate index entries */
        struct ext3_extent_idx *ei = EXT_FIRST_INDEX(eh);
        
        for (i = 0; i < ext2fs_le16_to_cpu(eh->eh_entries); i++, ei++) {
            if (!validate_extent_idx(ei, device_blocks))
                return 0;
        }
    }
    
    return 1;
}

/*
 * Scan inode extent header for residual index pointers
 * Even when eh_entries is reduced, memmove may leave old pointers beyond
 * Returns number of potentially valid residual pointers found
 */
int scan_residual_extent_pointers(struct ext3_extent_header *eh, 
                                  int blocksize,
                                  blk64_t *residual_blocks,
                                  int max_residual)
{
    struct ext3_extent_idx *ei;
    int i, count = 0;
    int max_entries = calculate_max_entries(blocksize, 0); /* Index block */
    
    if (!eh || eh->eh_depth == 0)
        return 0; /* Only applies to index blocks */
    
    /* Start scanning from eh_entries to eh_max */
    ei = EXT_FIRST_INDEX(eh);
    ei += ext2fs_le16_to_cpu(eh->eh_entries);
    
    for (i = ext2fs_le16_to_cpu(eh->eh_entries); 
         i < max_entries && count < max_residual; 
         i++, ei++) {
        blk64_t leaf = ext2fs_le32_to_cpu(ei->ei_leaf) +
                      ((__u64)ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32);
        
        /* Check if this looks like a valid block pointer */
        if (leaf > 0 && leaf < (1ULL << 48)) { /* 48-bit block number limit */
            residual_blocks[count++] = leaf;
        }
    }
    
    return count;
}
