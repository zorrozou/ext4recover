/*
 * parallel_scan.h - parallel aggressive scan entry point
 */
#ifndef _PARALLEL_SCAN_H
#define _PARALLEL_SCAN_H

#include <ext2fs/ext2fs.h>

struct recover_context;

/*
 * Parallel replacement for scan_for_extent_headers().
 * Reader thread does large sequential pread64(); N worker threads do
 * magic+sanity scanning per chunk; the calling thread acts as the single
 * writer that dumps hits in ascending block order (byte-identical to the
 * single-threaded scan).
 *
 * Returns number of orphaned extent blocks recovered, or -1 on setup error.
 */
int parallel_scan_for_extent_headers(struct recover_context *ctx,
                                     blk64_t start, blk64_t end,
                                     int n_workers);

/* Exposed from aggressive_scan_v5.c for the writer thread. */
int recover_orphaned_extent_block(struct recover_context *ctx,
                                  blk64_t block_num, char *buf);
int is_block_recovered(blk64_t block);

#endif
