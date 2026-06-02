/*
 * bigalloc_support.c - Bigalloc/Cluster support for ext4recover
 */

#include "ext4_common_v5.h"

/*
 * Initialize cluster/bigalloc information from superblock
 */
void init_cluster_info(struct recover_context *ctx)
{
    struct ext2_super_block *sb = ctx->fs->super;
    
    /* Check if bigalloc feature is enabled */
    ctx->has_bigalloc = (sb->s_feature_ro_compat & 
                         EXT4_FEATURE_RO_COMPAT_BIGALLOC) ? 1 : 0;
    
    if (ctx->has_bigalloc) {
        ctx->log_cluster_size = sb->s_log_cluster_size;
        /* cluster_size = block_size * 2^(s_log_cluster_size - s_log_block_size) */
        int log_block_size = sb->s_log_block_size;
        ctx->cluster_ratio = 1 << (ctx->log_cluster_size - log_block_size);
        
        LOG_INFO("Bigalloc enabled: cluster_ratio=%d (cluster_size=%d blocks)",
                 ctx->cluster_ratio, ctx->cluster_ratio);
    } else {
        ctx->cluster_ratio = 1;
        ctx->log_cluster_size = 0;
        LOG_DEBUG(ctx, "Bigalloc not enabled, using 1:1 cluster:block ratio");
    }
}

/*
 * Convert cluster number to physical block number
 */
blk64_t cluster_to_block(struct recover_context *ctx, blk64_t cluster)
{
    if (!ctx->has_bigalloc)
        return cluster;
    
    return cluster * ctx->cluster_ratio;
}

/*
 * Convert cluster length to block length
 */
blk64_t cluster_len_to_blocks(struct recover_context *ctx, __u16 cluster_len)
{
    if (!ctx->has_bigalloc)
        return cluster_len;
    
    return cluster_len * ctx->cluster_ratio;
}
