/*
 * journal_recovery.c - Journal (jbd2) parsing and recovery module
 * 
 * Recovery strategy for single-level extent files:
 * The journal records inode table blocks. When a file is deleted,
 * its extent info is cleared from the inode. But the journal may
 * still hold an older copy of the inode table block with the
 * original extent data intact.
 *
 * We scan ALL journal blocks (including those from "completed" transactions)
 * looking for:
 * 1. Inode table blocks - parse each inode within for extent info
 * 2. Standalone extent blocks (for multi-level extent trees)
 */

#include "ext4_common_v5.h"
#include <endian.h>

/* Byte order conversion macros */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define be32_to_cpu(x) __builtin_bswap32(x)
#define be16_to_cpu(x) __builtin_bswap16(x)
#define be64_to_cpu(x) __builtin_bswap64(x)
#else
#define be32_to_cpu(x) (x)
#define be16_to_cpu(x) (x)
#define be64_to_cpu(x) (x)
#endif

/* Journal structures */
typedef struct journal_header_s {
    __u32 h_magic;
    __u32 h_blocktype;
    __u32 h_sequence;
} journal_header_t;

typedef struct journal_block_tag3_s {
    __u32 t_blocknr;
    __u32 t_flags;
    __u32 t_blocknr_high;
    __u32 t_checksum;
} journal_block_tag3_t;

typedef struct journal_block_tag_s {
    __u32 t_blocknr;
    __u16 t_checksum;
    __u16 t_flags;
    __u32 t_blocknr_high;
} journal_block_tag_t;

typedef struct journal_superblock_s {
    journal_header_t s_header;
    __u32 s_blocksize;
    __u32 s_maxlen;
    __u32 s_first;
    __u32 s_sequence;
    __u32 s_start;
    __u32 s_errno;
    __u32 s_feature_compat;
    __u32 s_feature_incompat;
    __u32 s_feature_ro_compat;
    __u8  s_uuid[16];
    __u32 s_nr_users;
    __u32 s_dynsuper;
    __u32 s_max_transaction;
    __u32 s_max_trans_data;
    __u32 s_checksum_type;
    __u32 s_padding[42];
    __u32 s_checksum;
    __u32 s_users[16*48];
} journal_superblock_t;

/* Journal block types */
#define JBD2_DESCRIPTOR_BLOCK 1
#define JBD2_COMMIT_BLOCK     2
#define JBD2_SUPERBLOCK_V1    3
#define JBD2_SUPERBLOCK_V2    4
#define JBD2_REVOKE_BLOCK     5

/* Journal magic */
#define JBD2_MAGIC_NUMBER 0xc03b3998U

/* Journal tag flags */
#define JBD2_FLAG_ESCAPE      0x0001
#define JBD2_FLAG_SAME_UUID   0x0002
#define JBD2_FLAG_DELETED     0x0004
#define JBD2_FLAG_LAST_TAG    0x0008

/* Journal feature flags */


/* Track recovered inodes to avoid duplicates */
#define MAX_RECOVERED_INODES 65536

struct journal_scan_state {
    struct recover_context *ctx;
    int has_csum_v3;
    int tag_size;
    /* Track which inodes we already recovered (to pick the best version) */
    __u32 *recovered_inodes;
    __u64 *recovered_sizes;
    __u32 *recovered_seqs;    /* jbd2 h_sequence of best version */
    __u8  *recovered_live;    /* 1 = best version was a live (links>0) copy */
    int recovered_count;
    /* Inode table block range */
    blk64_t inode_table_start;
    blk64_t inode_table_end;
};

/*
 * A1: inode-table interval map.
 *
 * The audit-era calc_inode_from_block() assumed an inode table always
 * lives inside its owning block group (g = blk / blocks_per_group).
 * That is FALSE under flex_bg (default since e2fsprogs 1.41 / kernel
 * 2.6.28): all inode tables of a flex group are packed into the flex
 * group's first member, so a table block of group (leader+k), k>0,
 * physically sits in the leader group and the assumption misses it —
 * journal recovery silently skips every inode outside the leader
 * group's own table.
 *
 * Fix: precompute a sorted array of [start, end) -> group ranges for
 * every group's inode table, binary-search on lookup. O(groups) once,
 * O(log groups) per query. Serves both is_inode_table_block() and
 * calc_inode_from_block(), so the two can never disagree again.
 */
struct itable_range {
    blk64_t start;      /* first block of this group's inode table */
    blk64_t end;        /* exclusive */
    dgrp_t  group;
};

static struct itable_range *g_itable_map = NULL;
static int g_itable_count = 0;

static int itable_range_cmp(const void *a, const void *b)
{
    const struct itable_range *ra = a, *rb = b;
    if (ra->start < rb->start) return -1;
    if (ra->start > rb->start) return 1;
    return 0;
}

static int build_itable_map(struct recover_context *ctx)
{
    ext2_filsys fs = ctx->fs;
    dgrp_t groups = fs->group_desc_count;
    __u32 inodes_per_group = fs->super->s_inodes_per_group;
    __u32 inode_size = EXT2_INODE_SIZE(fs->super);
    blk64_t itb_blocks = ((blk64_t)inodes_per_group * inode_size +
                          fs->blocksize - 1) / fs->blocksize;

    free(g_itable_map);
    g_itable_map = malloc((size_t)groups * sizeof(struct itable_range));
    if (!g_itable_map) {
        g_itable_count = 0;
        return -1;
    }

    int n = 0;
    for (dgrp_t g = 0; g < groups; g++) {
        blk64_t start = ext2fs_inode_table_loc(fs, g);
        if (start == 0)
            continue;   /* uninitialized/bogus descriptor */
        g_itable_map[n].start = start;
        g_itable_map[n].end   = start + itb_blocks;
        g_itable_map[n].group = g;
        n++;
    }
    g_itable_count = n;
    qsort(g_itable_map, n, sizeof(struct itable_range), itable_range_cmp);
    return 0;
}

static void free_itable_map(void)
{
    free(g_itable_map);
    g_itable_map = NULL;
    g_itable_count = 0;
}

/* Binary search: range containing fs_block, or NULL. */
static struct itable_range *itable_lookup(blk64_t fs_block)
{
    int lo = 0, hi = g_itable_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fs_block < g_itable_map[mid].start)
            hi = mid - 1;
        else if (fs_block >= g_itable_map[mid].end)
            lo = mid + 1;
        else
            return &g_itable_map[mid];
    }
    return NULL;
}

/*
 * Check if a filesystem block number is an inode table block
 */
static int is_inode_table_block(struct journal_scan_state *state, blk64_t fs_block)
{
    (void)state;
    return itable_lookup(fs_block) != NULL;
}

/*
 * Journal version selection (A2 final form).
 *
 * A deleted file can appear in the journal as multiple inode copies:
 *   LIVE copies   (i_links_count > 0): states while the file existed -
 *                 these carry user-visible versions; newest seq wins
 *                 (audit-B2 semantics: a truncate+rewrite before the
 *                 delete must yield the NEW, possibly smaller content).
 *   ARTIFACT copies (i_links_count == 0): orphan/mid-truncate states
 *                 journaled BY the deletion itself. ext4_ext_truncate
 *                 restarts the handle on big files, journaling shrinking
 *                 extent subsets at HIGHER seq than any live copy. These
 *                 must never clobber a live recovery; among themselves
 *                 the largest (least-truncated) one wins.
 *
 * Selection table (R = best processed so far, C = candidate):
 *   C live,     R live     -> process iff C.seq > R.seq
 *   C live,     R artifact -> process (live supersedes artifacts)
 *   C artifact, R live     -> skip
 *   C artifact, R artifact -> process iff C.size > R.size
 */
static int should_skip_version(struct journal_scan_state *state, __u32 ino,
                               __u32 seq, __u64 size, int is_live)
{
    for (int i = 0; i < state->recovered_count; i++) {
        if (state->recovered_inodes[i] == ino) {
            int r_live = state->recovered_live[i];
            if (is_live && r_live)
                return state->recovered_seqs[i] >= seq;
            if (is_live && !r_live)
                return 0;
            if (!is_live && r_live)
                return 1;
            return state->recovered_sizes[i] >= size;
        }
    }
    return 0;
}

/* Record a processed version. Live entries supersede artifact entries;
 * within a family size/seq keep their maxima. size==0 means "output
 * file unchanged, just update bookkeeping". */
static void mark_version(struct journal_scan_state *state, __u32 ino,
                         __u64 size, __u32 seq, int is_live)
{
    for (int i = 0; i < state->recovered_count; i++) {
        if (state->recovered_inodes[i] == ino) {
            if (is_live && !state->recovered_live[i]) {
                state->recovered_live[i]  = 1;
                state->recovered_sizes[i] = size;
                state->recovered_seqs[i]  = seq;
                return;
            }
            if (size > state->recovered_sizes[i])
                state->recovered_sizes[i] = size;
            if (seq > state->recovered_seqs[i])
                state->recovered_seqs[i] = seq;
            return;
        }
    }
    if (state->recovered_count < MAX_RECOVERED_INODES) {
        state->recovered_inodes[state->recovered_count] = ino;
        state->recovered_sizes[state->recovered_count]  = size;
        state->recovered_seqs[state->recovered_count]   = seq;
        state->recovered_live[state->recovered_count]   = is_live ? 1 : 0;
        state->recovered_count++;
    }
}

/*
 * Calculate inode number from block number and offset within block
 */
static __u32 calc_inode_from_block(struct recover_context *ctx,
                                    blk64_t fs_block, int offset_in_block)
{
    /* A1: flex_bg-safe O(log groups) lookup via the interval map.
     * (The previous O(1) "g = blk / blocks_per_group" shortcut broke on
     * flex_bg disks where inode tables live in the flex-group leader.) */
    ext2_filsys fs = ctx->fs;
    __u32 inode_size = EXT2_INODE_SIZE(fs->super);
    __u32 inodes_per_group = fs->super->s_inodes_per_group;

    struct itable_range *r = itable_lookup(fs_block);
    if (!r)
        return 0;

    blk64_t block_offset = fs_block - r->start;
    int inodes_per_block = fs->blocksize / inode_size;
    __u32 local_inode = block_offset * inodes_per_block +
                       offset_in_block / inode_size;
    if (local_inode >= inodes_per_group)
        return 0;
    return r->group * inodes_per_group + local_inode + 1;
}

/*
 * B2: read a metadata (extent tree) block as it looked AT OR BEFORE
 * transaction `seq`. The journal's versioned copy is authoritative for
 * a deleted file's tree: the on-disk block may have been reused by a
 * newer file while still carrying a valid-looking extent magic. Falls
 * back to the raw disk when the journal holds no copy.
 * Returns 0 on success (buf filled), -1 if neither source works.
 */
static int read_tree_block_versioned(struct recover_context *ctx,
                                     blk64_t blk, __u32 seq, char *buf)
{
    if (journal_index_read(ctx, blk, seq, buf) > 0) {
        struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
        if (ext2fs_le16_to_cpu(eh->eh_magic) == EXT4_EXT_MAGIC)
            return 0;
    }
    if (pread(ctx->device_fd, buf, ctx->blocksize,
              (off_t)blk * ctx->blocksize) == ctx->blocksize) {
        struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
        if (ext2fs_le16_to_cpu(eh->eh_magic) == EXT4_EXT_MAGIC)
            return 0;
    }
    return -1;
}

/*
 * Try to recover a file from extent data found in journal inode
 */
static int recover_inode_from_journal(struct recover_context *ctx,
                                       __u32 ino,
                                       struct ext2_inode_large *jinode,
                                       __u32 seq)
{
    struct ext3_extent_header *eh;
    struct ext3_extent *ee;
    int fd;
    int retval;
    char filename[512];
    
    eh = (struct ext3_extent_header *)jinode->i_block;
    
    /* Validate extent header */
    if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT4_EXT_MAGIC)
        return -1;
    
    __u16 entries = ext2fs_le16_to_cpu(eh->eh_entries);
    __u16 max_ent = ext2fs_le16_to_cpu(eh->eh_max);
    __u16 depth = ext2fs_le16_to_cpu(eh->eh_depth);
    
    if (entries == 0 || entries > max_ent)
        return -1;
    if (max_ent > 4 && max_ent > (60 - sizeof(struct ext3_extent_header)) / sizeof(struct ext3_extent))
        return -1;  /* In-inode extent max based on i_block[15] = 60 bytes */
    if (depth > 5)
        return -1;
    
    /* For now handle leaf (depth=0) directly - this is the common case */
    if (depth != 0) {
        /* Multi-level: inode contains index entries pointing to leaf blocks on disk.
         * Those leaf blocks were freed but may still be physically intact. */
        LOG_DEBUG(ctx, "Journal inode %u has depth=%u extent tree, reading child blocks", ino, depth);
        
        /* First verify at least one child block is still reachable
         * (journal version <= seq preferred, disk fallback). */
        struct ext3_extent_idx *idx_check = (struct ext3_extent_idx *)(eh + 1);
        int any_valid = 0;
        for (int i = 0; i < entries; i++, idx_check++) {
            blk64_t child_blk = ext2fs_le32_to_cpu(idx_check->ei_leaf) +
                ((__u64)ext2fs_le16_to_cpu(idx_check->ei_leaf_hi) << 32);
            if (child_blk == 0 || child_blk >= ctx->total_blocks) continue;

            char *vbuf;
            if (ext2fs_get_mem(ctx->blocksize, &vbuf)) continue;
            if (read_tree_block_versioned(ctx, child_blk, seq, vbuf) == 0) {
                struct ext3_extent_header *veh = (struct ext3_extent_header *)vbuf;
                if (ext2fs_le16_to_cpu(veh->eh_entries) > 0) {
                    any_valid = 1;
                    ext2fs_free_mem(&vbuf);
                    break;
                }
            }
            ext2fs_free_mem(&vbuf);
        }
        
        if (!any_valid) {
            LOG_DEBUG(ctx, "Multi-level inode %u: no valid child blocks on disk, skipping", ino);
            return -1;
        }

        /* A2: fully-covered pre-check — ARTIFACT copies only. A live
         * copy may legitimately reuse the same physical blocks for new
         * content (truncate+rewrite), so live copies always rewrite.
         * Artifacts never carry content beyond what's already dumped. */
        if (jinode->i_links_count == 0 && ctx->recovered_extents) {
            int total_pre = 0, fully_pre = 0;
            struct ext3_extent_idx *idx_pre = (struct ext3_extent_idx *)(eh + 1);
            for (int i = 0; i < entries; i++, idx_pre++) {
                blk64_t child_blk = ext2fs_le32_to_cpu(idx_pre->ei_leaf) +
                    ((__u64)ext2fs_le16_to_cpu(idx_pre->ei_leaf_hi) << 32);
                if (child_blk == 0 || child_blk >= ctx->total_blocks) continue;
                char *pbuf;
                if (ext2fs_get_mem(ctx->blocksize, &pbuf)) continue;
                if (pread(ctx->device_fd, pbuf, ctx->blocksize,
                          (off_t)child_blk * ctx->blocksize) != ctx->blocksize) {
                    ext2fs_free_mem(&pbuf);
                    continue;
                }
                struct ext3_extent_header *peh = (struct ext3_extent_header *)pbuf;
                if (ext2fs_le16_to_cpu(peh->eh_magic) == EXT4_EXT_MAGIC &&
                    peh->eh_depth == 0) {
                    struct ext3_extent *pee = (struct ext3_extent *)(peh + 1);
                    int pn = ext2fs_le16_to_cpu(peh->eh_entries);
                    for (int j = 0; j < pn; j++, pee++) {
                        __u16 plen = ext2fs_le16_to_cpu(pee->ee_len);
                        __u64 pst = ((__u64)ext2fs_le16_to_cpu(pee->ee_start_hi) << 32) +
                                    ext2fs_le32_to_cpu(pee->ee_start);
                        if (plen > 32768) plen -= 32768;
                        if (plen == 0 || pst == 0) continue;
                        total_pre++;
                        if (intervals_query(ctx->recovered_extents,
                                            (uint64_t)pst, (uint64_t)plen) == 1)
                            fully_pre++;
                    }
                }
                ext2fs_free_mem(&pbuf);
            }
            if (total_pre > 0 && fully_pre == total_pre) {
                ctx->dedup_skipped++;
                LOG_DEBUG(ctx, "Journal multi-level inode %u fully covered, skip", ino);
                return 1;   /* processed; existing output kept */
            }
        }
        
        /* Write to temp file first - only promote to main file if larger */
        char filename_ml[512], tmpname_ml[512];
        char base_ml[300];
        resolve_output_name(ctx, ino, base_ml, sizeof(base_ml));
        snprintf(filename_ml, sizeof(filename_ml), "%s/%s", ctx->recover_dir, base_ml);
        snprintf(tmpname_ml, sizeof(tmpname_ml), "%s/.%u_tmp", ctx->recover_dir, ino);
        int fd_ml = open(tmpname_ml, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
        if (fd_ml < 0) {
            LOG_ERROR("Failed to create temp recovery file for inode %u", ino);
            return -1;
        }
        
        int recovered_blocks_ml = 0;
        struct ext3_extent_idx *idx = (struct ext3_extent_idx *)(eh + 1);
        
        for (int i = 0; i < entries; i++, idx++) {
            blk64_t child_blk = ext2fs_le32_to_cpu(idx->ei_leaf) +
                ((__u64)ext2fs_le16_to_cpu(idx->ei_leaf_hi) << 32);
            
            if (child_blk == 0 || child_blk >= ctx->total_blocks)
                continue;

            /* B2: versioned read - journal copy (<= seq) first, disk
             * fallback. Recovers trees whose leaves were reused on disk. */
            char *cbuf;
            retval = ext2fs_get_mem(ctx->blocksize, &cbuf);
            if (retval) continue;

            if (read_tree_block_versioned(ctx, child_blk, seq, cbuf) != 0) {
                ext2fs_free_mem(&cbuf);
                continue;
            }

            struct ext3_extent_header *ceh = (struct ext3_extent_header *)cbuf;
            
            if (ceh->eh_depth == 0) {
                /* Leaf block - recover all extents */
                struct ext3_extent *cee = (struct ext3_extent *)(ceh + 1);
                int leaf_entries = ext2fs_le16_to_cpu(ceh->eh_entries);
                for (int j = 0; j < leaf_entries; j++, cee++) {
                    __u32 blk_num = ext2fs_le32_to_cpu(cee->ee_block);
                    __u16 blk_len = ext2fs_le16_to_cpu(cee->ee_len);
                    __u64 phys = ((__u64)ext2fs_le16_to_cpu(cee->ee_start_hi) << 32) +
                                 ext2fs_le32_to_cpu(cee->ee_start);
                    if (blk_len > 32768) blk_len -= 32768; /* uninit */
                    if (blk_len == 0 || phys == 0) continue;
                    if (phys + blk_len > ctx->total_blocks) continue;
                    if (recover_block_to_file(ctx->device_fd, fd_ml,
                                            blk_num, blk_len, phys, ctx->blocksize, ctx)) {
                        recovered_blocks_ml += blk_len;
                    }
                }
            } else {
                /* Another level of indices - handle depth=2 */
                struct ext3_extent_idx *idx2 = (struct ext3_extent_idx *)(ceh + 1);
                int idx2_entries = ext2fs_le16_to_cpu(ceh->eh_entries);
                for (int j = 0; j < idx2_entries; j++, idx2++) {
                    blk64_t leaf_blk = ext2fs_le32_to_cpu(idx2->ei_leaf) +
                        ((__u64)ext2fs_le16_to_cpu(idx2->ei_leaf_hi) << 32);
                    if (leaf_blk == 0 || leaf_blk >= ctx->total_blocks) continue;

                    char *lbuf;
                    if (ext2fs_get_mem(ctx->blocksize, &lbuf)) continue;
                    if (read_tree_block_versioned(ctx, leaf_blk, seq, lbuf) != 0) {
                        ext2fs_free_mem(&lbuf);
                        continue;
                    }

                    struct ext3_extent_header *leh = (struct ext3_extent_header *)lbuf;
                    if (leh->eh_depth == 0) {
                        struct ext3_extent *lee = (struct ext3_extent *)(leh + 1);
                        int l_entries = ext2fs_le16_to_cpu(leh->eh_entries);
                        for (int k = 0; k < l_entries; k++, lee++) {
                            __u32 blk_num = ext2fs_le32_to_cpu(lee->ee_block);
                            __u16 blk_len = ext2fs_le16_to_cpu(lee->ee_len);
                            __u64 phys = ((__u64)ext2fs_le16_to_cpu(lee->ee_start_hi) << 32) +
                                         ext2fs_le32_to_cpu(lee->ee_start);
                            if (blk_len > 32768) blk_len -= 32768;
                            if (blk_len == 0 || phys == 0) continue;
                            if (phys + blk_len > ctx->total_blocks) continue;
                            if (recover_block_to_file(ctx->device_fd, fd_ml,
                                                    blk_num, blk_len, phys, ctx->blocksize, ctx)) {
                                recovered_blocks_ml += blk_len;
                            }
                        }
                    }
                    ext2fs_free_mem(&lbuf);
                }
            }
            ext2fs_free_mem(&cbuf);
        }
        
        close(fd_ml);
        
        if (recovered_blocks_ml > 0) {
            __u64 orig_size = ((__u64)jinode->i_size_high << 32) | jinode->i_size;
            if (orig_size > 0 && orig_size < (__u64)recovered_blocks_ml * ctx->blocksize) {
                if (truncate(tmpname_ml, orig_size) != 0)
                    LOG_DEBUG(ctx, "truncate(%s) failed: %s", tmpname_ml, strerror(errno));
            }
            /* Selection layer (should_skip_version) already decided this
             * version must win; replace unconditionally (audit-B2). */
            if (rename(tmpname_ml, filename_ml) != 0) {
                LOG_ERROR("rename(%s -> %s) failed: %s", tmpname_ml, filename_ml,
                         strerror(errno));
                unlink(tmpname_ml);
                return -1;
            }
            LOG_INFO("Journal recovered inode %u (multi-level depth=%u): %d blocks (%llu bytes) -> %s",
                    ino, depth, recovered_blocks_ml, (unsigned long long)orig_size, filename_ml);
            ctx->journal_recovered++;
            return 0;
        } else {
            unlink(tmpname_ml);
            return -1;
        }
    }
    
    /* Depth == 0: leaf extents are directly in inode */
    /* A2 pre-check: artifact copies only (see multi-level comment). */
    if (jinode->i_links_count == 0 && ctx->recovered_extents) {
        struct ext3_extent *pe = (struct ext3_extent *)(eh + 1);
        int total = 0, fully = 0;
        for (int k = 0; k < entries; k++, pe++) {
            __u16 plen = ext2fs_le16_to_cpu(pe->ee_len);
            __u64 pst  = (((__u64)ext2fs_le16_to_cpu(pe->ee_start_hi) << 32) +
                          (__u64)ext2fs_le32_to_cpu(pe->ee_start));
            if (plen > 32768) plen -= 32768;
            if (plen == 0 || pst == 0) continue;
            total++;
            if (intervals_query(ctx->recovered_extents,
                                (uint64_t)pst, (uint64_t)plen) == 1)
                fully++;
        }
        if (total > 0 && fully == total) {
            LOG_DEBUG(ctx, "Journal leaf for inode %u fully covered, skip", ino);
            return 1;   /* processed; existing output kept */
        }
    }
    /* Write to temp file, promote only if larger than existing */
    char tmpname[512];
    char base_d0[300];
    resolve_output_name(ctx, ino, base_d0, sizeof(base_d0));
    snprintf(filename, sizeof(filename), "%s/%s", ctx->recover_dir, base_d0);
    snprintf(tmpname, sizeof(tmpname), "%s/.%u_d0tmp", ctx->recover_dir, ino);
    fd = open(tmpname, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
    if (fd < 0) {
        LOG_ERROR("Failed to create temp recovery file: %s", tmpname);
        return -1;
    }
    
    ee = (struct ext3_extent *)(eh + 1);
    int recovered_blocks = 0;
    
    for (int i = 0; i < entries; i++, ee++) {
        __u32 ee_block = ext2fs_le32_to_cpu(ee->ee_block);
        __u16 ee_len = ext2fs_le16_to_cpu(ee->ee_len);
        __u64 ee_start = ((__u64)ext2fs_le16_to_cpu(ee->ee_start_hi) << 32) +
                         ext2fs_le32_to_cpu(ee->ee_start);
        
        /* Handle uninitialized extents */
        if (ee_len > 32768) {
            ee_len -= 32768;
        }
        
        if (ee_len == 0 || ee_start == 0)
            continue;
        
        /* Validate physical block is within device */
        if (ee_start + ee_len > ctx->total_blocks) {
            LOG_DEBUG(ctx, "  Extent %d out of range: start=%llu len=%u total=%llu",
                     i, (unsigned long long)ee_start, ee_len, 
                     (unsigned long long)ctx->total_blocks);
            continue;
        }
        
        LOG_DEBUG(ctx, "  Recovering extent: logical=%u, len=%u, phys=%llu",
                 ee_block, ee_len, (unsigned long long)ee_start);
        
        if (!recover_block_to_file(ctx->device_fd, fd, ee_block, ee_len,
                                  ee_start, ctx->blocksize, ctx)) {
            LOG_WARN("Failed to recover extent %d for inode %u", i, ino);
        } else {
            recovered_blocks += ee_len;
        }
    }
    
    close(fd);
    
    if (recovered_blocks > 0) {
        /* Truncate to original file size if known */
        __u64 orig_size = ((__u64)jinode->i_size_high << 32) | jinode->i_size;
        if (orig_size > 0 && orig_size < (__u64)recovered_blocks * ctx->blocksize) {
            if (truncate(tmpname, orig_size) != 0)
                LOG_DEBUG(ctx, "truncate(%s) failed: %s", tmpname, strerror(errno));
        }

        /* Selection layer already decided; replace unconditionally. */
        if (rename(tmpname, filename) != 0) {
            LOG_ERROR("rename(%s -> %s) failed: %s", tmpname, filename,
                     strerror(errno));
            unlink(tmpname);
            return -1;
        }
        LOG_INFO("Journal recovered inode %u: %d blocks (%llu bytes) -> %s",
                ino, recovered_blocks, (unsigned long long)orig_size, filename);
        ctx->journal_recovered++;
        return 0;
    } else {
        /* Remove empty file */
        unlink(tmpname);
        return -1;
    }
}

/*
 * Process an inode table block found in the journal
 */
static int process_inode_table_block(struct journal_scan_state *state,
                                      char *buf, blk64_t fs_block,
                                      __u32 seq)
{
    struct recover_context *ctx = state->ctx;
    __u32 inode_size = EXT2_INODE_SIZE(ctx->fs->super);
    int inodes_in_block = ctx->blocksize / inode_size;
    int recovered = 0;
    
    LOG_DEBUG(ctx, "Processing inode table block %llu (%d inodes)",
             (unsigned long long)fs_block, inodes_in_block);
    
    for (int i = 0; i < inodes_in_block; i++) {
        struct ext2_inode_large *jinode = (struct ext2_inode_large *)(buf + i * inode_size);
        
        /* Skip non-regular files */
        if (!LINUX_S_ISREG(jinode->i_mode))
            continue;
        
        /* Skip if no data (size=0 or no blocks) */
        __u64 file_size = ((__u64)jinode->i_size_high << 32) | jinode->i_size;
        if (file_size == 0)
            continue;
        
        /* Check if this inode uses extents (flag 0x80000) */
        if (!(jinode->i_flags & EXT4_EXTENTS_FL)) {
            /* C6: inline_data — tiny files stored in i_block */
            if (ctx->caps.has_inline_data &&
                (jinode->i_flags & EXT4_INLINE_DATA_FL)) {
                __u32 ino = calc_inode_from_block(ctx, fs_block, i * inode_size);
                if (ino > EXT2_GOOD_OLD_FIRST_INO) {
                    struct ext2_inode cur;
                    if (ext2fs_read_inode(ctx->fs, ino, &cur) == 0 &&
                        cur.i_links_count == 0) {
                        if (should_skip_version(&state, ino, seq, file_size,
                                                jinode->i_links_count > 0))
                            goto next_inode;
                        if (recover_inline_data(ctx, ino, jinode) == 0) {
                            mark_version(&state, ino, file_size, seq,
                                        jinode->i_links_count > 0);
                        }
                    }
                }
            }
            /* C8: indirect block recovery for !extent old-format files */
            if (!(jinode->i_flags & EXT4_INLINE_DATA_FL) && file_size > 0) {
                __u32 ino = calc_inode_from_block(ctx, fs_block, i * inode_size);
                if (ino > EXT2_GOOD_OLD_FIRST_INO) {
                    struct ext2_inode cur;
                    if (ext2fs_read_inode(ctx->fs, ino, &cur) == 0 &&
                        cur.i_links_count == 0) {
                        if (!should_skip_version(&state, ino, seq, file_size,
                                                 jinode->i_links_count > 0)) {
                            if (recover_indirect_file(ctx, ino, jinode) == 0) {
                                mark_version(&state, ino, file_size, seq,
                                            jinode->i_links_count > 0);
                            }
                        }
                    }
                }
            }
            goto next_inode;
        }
        
        /* Check extent header magic */
        struct ext3_extent_header *eh = (struct ext3_extent_header *)jinode->i_block;
        if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT4_EXT_MAGIC)
            continue;
        
        if (ext2fs_le16_to_cpu(eh->eh_entries) == 0)
            continue;
        
        /* Calculate real inode number */
        __u32 ino = calc_inode_from_block(ctx, fs_block, i * inode_size);
        if (ino == 0)
            continue;
        
        /* Skip system inodes */
        if (ino <= EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO)
            continue;
        
        /* Check if current on-disk inode has been cleared (deleted) */
        struct ext2_inode cur_inode;
        errcode_t retval = ext2fs_read_inode(ctx->fs, ino, &cur_inode);
        if (retval)
            continue;
        
        /* Only recover if current inode is deleted (links=0, size=0 or dtime set) */
        int is_deleted = (cur_inode.i_links_count == 0) || 
                        (cur_inode.i_dtime != 0) ||
                        (cur_inode.i_size == 0 && file_size > 0);
        
        if (!is_deleted)
            continue;
        
        /* Version selection: live copies beat artifacts; see table at
         * should_skip_version(). is_live = the JOURNAL COPY's links. */
        int is_live = jinode->i_links_count > 0;
        if (should_skip_version(state, ino, seq, file_size, is_live))
            continue;

        LOG_DEBUG(ctx, "Found deleted inode %u in journal (seq=%u size=%llu entries=%u depth=%u links=%u)",
                 ino, seq, (unsigned long long)file_size,
                 ext2fs_le16_to_cpu(eh->eh_entries),
                 ext2fs_le16_to_cpu(eh->eh_depth),
                 jinode->i_links_count);

        /* Try to recover. Return: 0 = output (re)written, 1 = processed
         * but existing output kept (redundant version). */
        int r = recover_inode_from_journal(ctx, ino, jinode, seq);
        if (r == 0) {
            mark_version(state, ino, file_size, seq, is_live);
            recovered++;
        } else if (r == 1) {
            mark_version(state, ino, 0, seq, is_live);
        }
next_inode:;
    }

    return recovered;
}

/*
 * Check if a block looks like a standalone extent block (for multi-level trees)
 */
static int is_standalone_extent_block(char *buf, int blocksize)
{
    struct ext3_extent_header *eh = (struct ext3_extent_header *)buf;
    
    if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT4_EXT_MAGIC)
        return 0;
    
    __u16 entries = ext2fs_le16_to_cpu(eh->eh_entries);
    __u16 max_ent = ext2fs_le16_to_cpu(eh->eh_max);
    
    if (entries == 0 || entries > max_ent)
        return 0;
    
    /* Standalone extent blocks have larger max_entries than in-inode ones */
    int expected_max = (blocksize - sizeof(struct ext3_extent_header)) / 
                       sizeof(struct ext3_extent);
    
    /* Must be close to the block-based max (not the inode-based max of ~4) */
    if (max_ent > 4 && max_ent <= expected_max)
        return 1;
    
    return 0;
}

/*
 * Read a journal block by offset within the journal inode.
 * B1: uses the cached handle opened by init_journal() - the previous
 * version did ext2fs_file_open + llseek + read + close PER BLOCK,
 * i.e. ~500K open/close cycles for a default 1GB journal scanned twice.
 */
static int read_journal_block(struct recover_context *ctx, blk64_t journal_blk,
                              char *buf)
{
    unsigned int got;
    errcode_t retval;

    if (!ctx->journal_file)
        return -1;

    retval = ext2fs_file_llseek(ctx->journal_file,
                                (__u64)journal_blk * ctx->blocksize,
                                EXT2_SEEK_SET, NULL);
    if (retval)
        return -1;

    retval = ext2fs_file_read(ctx->journal_file, buf, ctx->blocksize, &got);
    if (retval || (int)got != ctx->blocksize)
        return -1;

    return 0;
}

/*
 * Initialize journal access
 */
int init_journal(struct recover_context *ctx)
{
    journal_superblock_t *jsb;
    char *buf;
    errcode_t retval;
    
    if (!ctx || !ctx->fs) {
        LOG_ERROR("Invalid context");
        return -1;
    }
    
    if (!(ctx->fs->super->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
        LOG_INFO("Filesystem does not have a journal");
        ctx->has_journal = 0;
        return 0;
    }
    
    ctx->has_journal = 1;
    
    if (ctx->fs->super->s_journal_inum == 0) {
        if (ctx->fs->super->s_journal_dev != 0) {
            LOG_WARN("External journal not supported");
        }
        return -1;
    }
    
    LOG_INFO("Journal is in inode %u", ctx->fs->super->s_journal_inum);
    ctx->journal_fd = -1;

    /* B1: open the journal inode once; all reads go through this. */
    retval = ext2fs_file_open(ctx->fs, ctx->fs->super->s_journal_inum, 0,
                              &ctx->journal_file);
    if (retval) {
        LOG_ERROR("Failed to open journal inode (error %ld)", (long)retval);
        ctx->journal_file = NULL;
        return -1;
    }

    retval = ext2fs_get_mem(ctx->blocksize, &buf);
    if (retval) return -1;
    
    /* Read journal superblock */
    if (read_journal_block(ctx, 0, buf) != 0) {
        LOG_ERROR("Failed to read journal superblock");
        ext2fs_free_mem(&buf);
        return -1;
    }
    
    jsb = (journal_superblock_t *)buf;
    
    if (be32_to_cpu(jsb->s_header.h_magic) != JBD2_MAGIC_NUMBER) {
        LOG_ERROR("Invalid journal magic: 0x%x", be32_to_cpu(jsb->s_header.h_magic));
        ext2fs_free_mem(&buf);
        return -1;
    }
    
    LOG_INFO("Journal superblock found:");
    LOG_INFO("  Block size: %u", be32_to_cpu(jsb->s_blocksize));
    LOG_INFO("  Total blocks: %u", be32_to_cpu(jsb->s_maxlen));
    LOG_INFO("  First block: %u", be32_to_cpu(jsb->s_first));
    LOG_INFO("  Sequence: %u", be32_to_cpu(jsb->s_sequence));
    LOG_INFO("  Start: %u", be32_to_cpu(jsb->s_start));
    LOG_INFO("  Feature incompat: 0x%x", be32_to_cpu(jsb->s_feature_incompat));
    
    ctx->journal_start = be32_to_cpu(jsb->s_first);
    ctx->journal_len = be32_to_cpu(jsb->s_maxlen);

    /* Feed jbd2 feature flags to the capability layer (Phase 0.1) */
    fs_capabilities_set_journal(ctx, be32_to_cpu(jsb->s_feature_incompat));

    ext2fs_free_mem(&buf);
    return 0;
}

/*
 * Scan entire journal for recoverable data
 * 
 * Strategy: Walk through ALL journal blocks sequentially.
 * For each descriptor block, follow its tags to find the corresponding
 * data blocks. Check each data block:
 * - If it maps to an inode table block → parse inodes for deleted files
 * - If it looks like a standalone extent block → save for recovery
 */
int recover_from_journal(struct recover_context *ctx)
{
    char *desc_buf = NULL;
    char *data_buf = NULL;
    blk64_t blk;
    errcode_t retval;
    
    if (!ctx->has_journal) {
        LOG_INFO("No journal available");
        return 0;
    }
    
    /* Allocate scan state */
    struct journal_scan_state state;
    memset(&state, 0, sizeof(state));
    state.ctx = ctx;

    /* A1: build the flex_bg-safe inode-table interval map once. */
    if (build_itable_map(ctx) != 0) {
        LOG_ERROR("Failed to build inode-table map");
        return -1;
    }

    state.recovered_inodes = calloc(MAX_RECOVERED_INODES, sizeof(__u32));
    state.recovered_sizes = calloc(MAX_RECOVERED_INODES, sizeof(__u64));
    state.recovered_seqs  = calloc(MAX_RECOVERED_INODES, sizeof(__u32));
    state.recovered_live  = calloc(MAX_RECOVERED_INODES, sizeof(__u8));
    if (!state.recovered_inodes || !state.recovered_sizes ||
        !state.recovered_seqs || !state.recovered_live) {
        LOG_ERROR("Failed to allocate inode tracking array");
        free(state.recovered_inodes);
        free(state.recovered_sizes); free(state.recovered_seqs);
        free(state.recovered_live);
        return -1;
    }
    
    retval = ext2fs_get_mem(ctx->blocksize, &desc_buf);
    if (retval) { free(state.recovered_inodes); free(state.recovered_sizes); free(state.recovered_seqs); free(state.recovered_live); return -1; }

    retval = ext2fs_get_mem(ctx->blocksize, &data_buf);
    if (retval) { ext2fs_free_mem(&desc_buf); free(state.recovered_inodes); free(state.recovered_sizes); free(state.recovered_seqs); free(state.recovered_live); return -1; }
    
    LOG_INFO("Starting journal recovery scan...");
    LOG_INFO("Scanning %llu journal blocks (brute-force, ignoring sequence)",
            (unsigned long long)ctx->journal_len);
    
    /*
     * Brute-force approach: scan every journal block.
     * When we find a descriptor block, parse its tags and read the
     * following data blocks, mapping them to filesystem blocks.
     */
    for (blk = ctx->journal_start; blk < ctx->journal_len; blk++) {
        if (blk % 1000 == 0 && blk > ctx->journal_start) {
            LOG_INFO("Scanned %llu / %llu journal blocks...",
                    (unsigned long long)blk,
                    (unsigned long long)ctx->journal_len);
        }
        
        if (read_journal_block(ctx, blk, desc_buf) != 0)
            continue;
        
        /* Check if this is a descriptor block */
        journal_header_t *header = (journal_header_t *)desc_buf;
        if (be32_to_cpu(header->h_magic) != JBD2_MAGIC_NUMBER)
            continue;
        if (be32_to_cpu(header->h_blocktype) != JBD2_DESCRIPTOR_BLOCK)
            continue;
        
        __u32 seq = be32_to_cpu(header->h_sequence);
        LOG_DEBUG(ctx, "Found descriptor block at journal offset %llu, seq %u",
                 (unsigned long long)blk, seq);
        
        /* Parse tags in this descriptor block.
         *
         * A5: tag wire format is decided ONLY by the journal sb feature
         * flags (mirrors fs/jbd2/journal.c::journal_tag_bytes and
         * recovery.c::do_one_pass). The old code guessed the format per
         * tag from a flags-value heuristic, which mis-sized csum_v3
         * tags (16 bytes, walked as 12) and silently dropped every tag
         * after the first one in multi-tag descriptors. */
        int tag_bytes = ctx->caps.journal_probed ? ctx->caps.jbd2_tag_bytes : 12;
        int tag_fmt   = ctx->caps.journal_probed ? ctx->caps.jbd2_tag_fmt
                                                 : JBD2_TAG_FMT_V1;
        char *tagp = desc_buf + sizeof(journal_header_t);
        int remaining = ctx->blocksize - sizeof(journal_header_t);
        blk64_t data_blk = blk + 1;  /* Data blocks follow descriptor */

        while (remaining >= tag_bytes) {
            blk64_t fs_block;
            __u32 flags;

            if (tag_fmt == JBD2_TAG_FMT_CSUM_V3) {
                journal_block_tag3_t *tag3 = (journal_block_tag3_t *)tagp;
                fs_block = be32_to_cpu(tag3->t_blocknr);
                flags    = be32_to_cpu(tag3->t_flags);
                if (ctx->caps.jbd2_has_64bit)
                    fs_block |= (blk64_t)be32_to_cpu(tag3->t_blocknr_high) << 32;
            } else {
                journal_block_tag_t *tag = (journal_block_tag_t *)tagp;
                fs_block = be32_to_cpu(tag->t_blocknr);
                flags    = be16_to_cpu(tag->t_flags);
                if (ctx->caps.jbd2_has_64bit)
                    fs_block |= (blk64_t)be32_to_cpu(tag->t_blocknr_high) << 32;
            }

            tagp += tag_bytes;
            remaining -= tag_bytes;
            if (!(flags & JBD2_FLAG_SAME_UUID)) {
                tagp += 16;       /* UUID follows this tag */
                remaining -= 16;
            }
            
            /* Skip if block number is unreasonable */
            if (fs_block == 0 || fs_block >= ctx->total_blocks) {
                data_blk++;
                if (flags & JBD2_FLAG_LAST_TAG) break;
                continue;
            }
            
            /* Read the corresponding data block from journal */
            if (data_blk >= ctx->journal_len) break;

            /* B2: record fs_block->journal_blk mapping in-flight. */
            if (ctx->jindex && fs_block != 0 && fs_block < ctx->total_blocks)
                jindex_add(ctx->jindex, fs_block, (__u32)data_blk, seq,
                           flags & JBD2_FLAG_ESCAPE);

            if (read_journal_block(ctx, data_blk, data_buf) != 0) {
                data_blk++;
                if (flags & JBD2_FLAG_LAST_TAG) break;
                continue;
            }
            
            /* Handle escaped blocks (magic number was escaped) */
            if (flags & JBD2_FLAG_ESCAPE) {
                /* Restore the magic number */
                *(__u32 *)data_buf = __builtin_bswap32(JBD2_MAGIC_NUMBER);
            }
            
            /* Check what kind of block this is */
            if (is_inode_table_block(&state, fs_block)) {
                process_inode_table_block(&state, data_buf, fs_block, seq);
            } else if (is_standalone_extent_block(data_buf, ctx->blocksize)) {
                /* Standalone extent block - save for later use */
                LOG_DEBUG(ctx, "Found standalone extent block (fs_block %llu) in journal",
                         (unsigned long long)fs_block);
                /* TODO: correlate with inode to create proper recovery file */
            }
            
            data_blk++;
            
            if (flags & JBD2_FLAG_LAST_TAG)
                break;
        }
        
        /* Skip past the data blocks we just processed */
        blk = data_blk - 1; /* -1 because loop will increment */
    }
    
    LOG_INFO("Journal scan complete. Recovered %lu files from journal.",
            ctx->journal_recovered);

    /* B2: index is now fully populated; sort once for binary search. */
    if (ctx->jindex) {
        jindex_sort(ctx->jindex);
        LOG_INFO("Journal block index: %d versioned copies mapped",
                 ctx->jindex->count);
    }

    ext2fs_free_mem(&desc_buf);
    ext2fs_free_mem(&data_buf);
    free(state.recovered_inodes);
    free(state.recovered_sizes); free(state.recovered_seqs);
    free(state.recovered_live);
    free_itable_map();

    return 0;
}

/*
 * Clean up journal resources
 */
void close_journal(struct recover_context *ctx)
{
    if (ctx->journal_file) {
        ext2fs_file_close(ctx->journal_file);
        ctx->journal_file = NULL;
    }
    if (ctx->journal_fd >= 0) {
        close(ctx->journal_fd);
        ctx->journal_fd = -1;
    }
}
