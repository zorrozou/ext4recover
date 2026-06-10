/*
 * journal_index.h - B2: fs_block -> journal copy multimap
 *
 * jbd2 is a write-ahead log: descriptor tags carry the destination fs
 * block number of every journaled data block, so the journal is a
 * versioned (by h_sequence) historical snapshot of metadata blocks.
 *
 * This index lets any recovery path read "the journal's copy of fs
 * block X" (optionally bounded by a max sequence), with the raw disk
 * as fallback. Primary consumer today: multi-level extent recovery,
 * whose child leaf blocks may already be reused on disk while a
 * pre-delete copy still lives in the journal. Later phases (revoke
 * targeting, fast-commit, filename versioning) reuse the same walk.
 */

#ifndef _JOURNAL_INDEX_H
#define _JOURNAL_INDEX_H

#include <ext2fs/ext2fs.h>

struct jindex_entry {
    blk64_t fs_block;       /* destination fs block */
    __u32   journal_blk;    /* offset inside journal (blocks) */
    __u32   seq;            /* jbd2 h_sequence of the transaction */
    __u8    escaped;        /* JBD2_FLAG_ESCAPE: restore magic on read */
};

struct journal_index {
    struct jindex_entry *e; /* sorted by (fs_block asc, seq asc) */
    int count;
    int cap;
};

struct recover_context;

/* Called once per descriptor tag during recover_from_journal() scan. */
int jindex_add(struct journal_index *ji, blk64_t fs_block,
               __u32 journal_blk, __u32 seq, int escaped);

/* Called once after the scan loop completes. */
void jindex_sort(struct journal_index *ji);

/* Read the journal copy of fs_block with the highest seq <= max_seq
 * (max_seq = 0 means "any/highest"). buf must hold one block.
 * Returns the seq of the copy used (>0), or 0 if none found/readable. */
__u32 journal_index_read(struct recover_context *ctx, blk64_t fs_block,
                         __u32 max_seq, char *buf);

void journal_index_free(struct recover_context *ctx);

#endif /* _JOURNAL_INDEX_H */
