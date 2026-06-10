/*
 * journal_index.c - B2: fs_block versioned index, built as side-effect
 * of recover_from_journal() main scan (zero extra journal passes).
 */

#include "ext4_common_v5.h"

static int jindex_cmp(const void *a, const void *b)
{
    const struct jindex_entry *ea = a, *eb = b;
    if (ea->fs_block < eb->fs_block) return -1;
    if (ea->fs_block > eb->fs_block) return 1;
    if (ea->seq < eb->seq) return -1;
    if (ea->seq > eb->seq) return 1;
    return 0;
}

int jindex_add(struct journal_index *ji, blk64_t fs_block,
               __u32 journal_blk, __u32 seq, int escaped)
{
    if (ji->count >= ji->cap) {
        int ncap = ji->cap ? ji->cap * 2 : 4096;
        struct jindex_entry *ne = realloc(ji->e, ncap * sizeof(*ne));
        if (!ne) return -1;
        ji->e = ne;
        ji->cap = ncap;
    }
    ji->e[ji->count].fs_block    = fs_block;
    ji->e[ji->count].journal_blk = journal_blk;
    ji->e[ji->count].seq         = seq;
    ji->e[ji->count].escaped     = escaped ? 1 : 0;
    ji->count++;
    return 0;
}

void jindex_sort(struct journal_index *ji)
{
    if (ji && ji->count > 0)
        qsort(ji->e, ji->count, sizeof(struct jindex_entry), jindex_cmp);
}

/* Allocate the index container. The actual entries are populated by
 * recover_from_journal() via jindex_add(); no second journal pass. */
int journal_index_build(struct recover_context *ctx)
{
    if (!ctx->has_journal) return 0;
    if (ctx->jindex) return 0;
    ctx->jindex = calloc(1, sizeof(struct journal_index));
    return ctx->jindex ? 0 : -1;
}

__u32 journal_index_read(struct recover_context *ctx, blk64_t fs_block,
                         __u32 max_seq, char *buf)
{
    struct journal_index *ji = ctx->jindex;
    if (!ji || ji->count == 0) return 0;

    int lo = 0, hi = ji->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ji->e[mid].fs_block < fs_block) lo = mid + 1;
        else hi = mid;
    }
    int best = -1;
    for (int i = lo; i < ji->count && ji->e[i].fs_block == fs_block; i++) {
        if (max_seq == 0 || ji->e[i].seq <= max_seq) best = i;
    }
    if (best < 0) return 0;
    if (!ctx->journal_file) return 0;

    unsigned int got;
    if (ext2fs_file_llseek(ctx->journal_file,
                           (__u64)ji->e[best].journal_blk * ctx->blocksize,
                           EXT2_SEEK_SET, NULL))
        return 0;
    if (ext2fs_file_read(ctx->journal_file, buf, ctx->blocksize, &got))
        return 0;
    if ((int)got != ctx->blocksize) return 0;

    if (ji->e[best].escaped)
        *(__u32 *)buf = __builtin_bswap32(0xc03b3998U);
    return ji->e[best].seq ? ji->e[best].seq : 1;
}

void journal_index_free(struct recover_context *ctx)
{
    if (ctx->jindex) {
        free(ctx->jindex->e);
        free(ctx->jindex);
        ctx->jindex = NULL;
    }
}
