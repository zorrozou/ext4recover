/*
 * fs_capabilities.c - On-disk format capability detection (Phase 0.1)
 *
 * See fs_capabilities.h for design rationale. This module is
 * read-only and additive: it changes no recovery behavior by itself;
 * it only gives later phases a single, consistent place to ask
 * "does this disk's format support X?".
 */

#include "ext4_common_v5.h"

void fs_capabilities_detect(struct recover_context *ctx)
{
    struct fs_capabilities *c = &ctx->caps;
    struct ext2_super_block *sb = ctx->fs->super;

    memset(c, 0, sizeof(*c));

    c->has_extents       = !!(sb->s_feature_incompat & EXT3_FEATURE_INCOMPAT_EXTENTS);
    c->has_flex_bg       = !!(sb->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FLEX_BG);
    c->has_64bit         = !!(sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT);
    c->has_inline_data   = !!(sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA);
    c->has_metadata_csum = !!(sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);
    c->has_orphan_file   = !!(sb->s_feature_compat & EXT4_FEATURE_COMPAT_ORPHAN_FILE);
    c->has_fc_compat     = !!(sb->s_feature_compat & EXT4_FEATURE_COMPAT_FAST_COMMIT);

    /* journal fields stay zero until init_journal() probes the jsb */
    c->journal_probed = 0;
    c->jbd2_tag_fmt   = JBD2_TAG_FMT_V1;
    c->jbd2_tag_bytes = 8;  /* v1/v2 tag without blocknr_high or UUID */
}

void fs_capabilities_set_journal(struct recover_context *ctx,
                                 __u32 jsb_feature_incompat)
{
    struct fs_capabilities *c = &ctx->caps;

    c->journal_probed = 1;
    c->jbd2_has_64bit = !!(jsb_feature_incompat & JBD2_FEATURE_INCOMPAT_64BIT_BLK);
    c->jbd2_has_fc    = !!(jsb_feature_incompat & JBD2_FEATURE_INCOMPAT_FAST_COMMIT_F);

    if (jsb_feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3_F)
        c->jbd2_tag_fmt = JBD2_TAG_FMT_CSUM_V3;
    else if (jsb_feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2)
        c->jbd2_tag_fmt = JBD2_TAG_FMT_CSUM_V2;
    else
        c->jbd2_tag_fmt = JBD2_TAG_FMT_V1;

    /*
     * Wire size of one tag, NOT counting the 16-byte UUID that follows
     * tags lacking JBD2_FLAG_SAME_UUID. Mirrors journal_tag_bytes() in
     * fs/jbd2/journal.c (stable since commit db9ee220361d, 2014):
     *   csum_v3 -> sizeof(journal_block_tag3_t) = 16
     *   else    -> sizeof(journal_block_tag_t)  = 12
     *              +2 if csum_v2, -4 if !64bit
     * i.e. v1: 8/12, csum_v2: 10/14 (32/64-bit), csum_v3: 16.
     */
    if (c->jbd2_tag_fmt == JBD2_TAG_FMT_CSUM_V3) {
        c->jbd2_tag_bytes = 16;
    } else {
        c->jbd2_tag_bytes = 12;
        if (c->jbd2_tag_fmt == JBD2_TAG_FMT_CSUM_V2)
            c->jbd2_tag_bytes += 2;
        if (!c->jbd2_has_64bit)
            c->jbd2_tag_bytes -= 4;
    }
}

static const char *onoff(int v) { return v ? "yes" : "no "; }

void fs_capabilities_print(struct recover_context *ctx)
{
    struct fs_capabilities *c = &ctx->caps;

    LOG_INFO("=== Disk format capabilities ===");
    LOG_INFO("  extents:       %s %s", onoff(c->has_extents),
             c->has_extents ? "" : "(ext3-style disk; extent phases degrade)");
    LOG_INFO("  flex_bg:       %s %s", onoff(c->has_flex_bg),
             c->has_flex_bg ? "(inode tables may live outside owning group)" : "");
    LOG_INFO("  64bit:         %s", onoff(c->has_64bit));
    LOG_INFO("  metadata_csum: %s %s", onoff(c->has_metadata_csum),
             c->has_metadata_csum ? "(csum attribution available)" : "(csum attribution off)");
    LOG_INFO("  inline_data:   %s", onoff(c->has_inline_data));
    LOG_INFO("  orphan_file:   %s %s", onoff(c->has_orphan_file),
             c->has_orphan_file ? "(s_last_orphan chain likely empty)" : "(classic orphan chain)");
    if (c->journal_probed) {
        LOG_INFO("  jbd2 tags:     %s (%d bytes)%s",
                 c->jbd2_tag_fmt == JBD2_TAG_FMT_CSUM_V3 ? "csum_v3" :
                 c->jbd2_tag_fmt == JBD2_TAG_FMT_CSUM_V2 ? "csum_v2" : "v1",
                 c->jbd2_tag_bytes,
                 c->jbd2_has_64bit ? ", 64bit" : "");
        LOG_INFO("  fast_commit:   %s %s", onoff(c->jbd2_has_fc),
                 c->jbd2_has_fc ? "(FC area parse available)" : "");
    } else {
        LOG_INFO("  jbd2:          (journal not probed)");
    }
}
