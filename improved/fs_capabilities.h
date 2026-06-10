/*
 * fs_capabilities.h - On-disk format capability detection (Phase 0.1)
 *
 * The tool runs against offline disks written by kernels of very
 * different eras (CentOS 5/6 era ext4, 3.x/4.x mainstream, 5.x,
 * 5.15+/6.x with orphan_file & fast_commit). The kernel version that
 * wrote the disk is unknowable; the ONLY reliable signal is the set
 * of feature flags in the ext4 superblock and the jbd2 journal
 * superblock.
 *
 * Every era-dependent recovery capability MUST gate on these flags
 * (never on heuristics), so that on old-format disks the tool
 * degrades exactly to the proven v5 behavior.
 */

#ifndef _FS_CAPABILITIES_H
#define _FS_CAPABILITIES_H

#include <ext2fs/ext2fs.h>

/* Fallback definitions for older libext2fs headers. Values are the
 * on-disk constants from fs/ext4/ext4.h and are stable ABI. */
#ifndef EXT4_FEATURE_COMPAT_ORPHAN_FILE
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE     0x1000
#endif
#ifndef EXT4_FEATURE_COMPAT_FAST_COMMIT
#define EXT4_FEATURE_COMPAT_FAST_COMMIT     0x0400
#endif
#ifndef EXT4_FEATURE_RO_COMPAT_METADATA_CSUM
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#endif
#ifndef EXT2_FEATURE_INCOMPAT_FLEX_BG
#define EXT2_FEATURE_INCOMPAT_FLEX_BG       0x0200
#endif
#ifndef EXT4_FEATURE_INCOMPAT_64BIT
#define EXT4_FEATURE_INCOMPAT_64BIT         0x0080
#endif
#ifndef EXT4_FEATURE_INCOMPAT_INLINE_DATA
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA   0x8000
#endif
#ifndef EXT3_FEATURE_INCOMPAT_EXTENTS
#define EXT3_FEATURE_INCOMPAT_EXTENTS       0x0040
#endif

/* jbd2 journal superblock incompat features (fs/jbd2 on-disk ABI) */
#define JBD2_FEATURE_INCOMPAT_REVOKE        0x00000001
#define JBD2_FEATURE_INCOMPAT_64BIT_BLK     0x00000002
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT  0x00000004
#define JBD2_FEATURE_INCOMPAT_CSUM_V2       0x00000008
#define JBD2_FEATURE_INCOMPAT_CSUM_V3_F     0x00000010
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT_F 0x00000020

/* jbd2 descriptor tag wire formats, decided ONLY by journal sb flags */
enum jbd2_tag_format {
    JBD2_TAG_FMT_V1 = 1,    /* journal_block_tag_t, no csum    */
    JBD2_TAG_FMT_CSUM_V2,   /* journal_block_tag_t + csum_v2   */
    JBD2_TAG_FMT_CSUM_V3,   /* journal_block_tag3_t (16 bytes) */
};

struct fs_capabilities {
    /* --- ext4 superblock derived --- */
    int has_extents;        /* INCOMPAT_EXTENTS: pure-ext3 disks lack it   */
    int has_flex_bg;        /* inode tables may live outside owning group  */
    int has_64bit;          /* >2^32 blocks                                 */
    int has_metadata_csum;  /* enables C3 csum attribution                  */
    int has_inline_data;    /* enables C6 inline-data recovery              */
    int has_orphan_file;    /* enables C4; s_last_orphan likely unused      */
    int has_fc_compat;      /* fs-level COMPAT_FAST_COMMIT flag             */

    /* --- jbd2 journal superblock derived (0 until journal read) --- */
    int journal_probed;     /* 1 once journal sb flags were read            */
    int jbd2_tag_fmt;       /* enum jbd2_tag_format                         */
    int jbd2_has_64bit;     /* tags carry t_blocknr_high                    */
    int jbd2_has_fc;        /* journal has a fast-commit area (C2)          */
    int jbd2_tag_bytes;     /* tag size on the wire (without UUID)          */
};

struct recover_context; /* fwd */

/* Detect fs-level capabilities from the (already open) superblock. */
void fs_capabilities_detect(struct recover_context *ctx);

/* Fill jbd2-level fields from the journal superblock incompat flags.
 * Called by init_journal() once it has the journal sb in hand. */
void fs_capabilities_set_journal(struct recover_context *ctx,
                                 __u32 jsb_feature_incompat);

/* Print the capability table so the user can see, for this disk,
 * which recovery strategies are live and which degrade. */
void fs_capabilities_print(struct recover_context *ctx);

#endif /* _FS_CAPABILITIES_H */
