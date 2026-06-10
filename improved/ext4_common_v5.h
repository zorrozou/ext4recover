/*
 * ext4_common.h - Common definitions for ext4recover v0.5
 * 
 * Enhanced with:
 * - Bigalloc/Cluster support
 * - Directory entry recovery
 * - Checkpoint/resume capability
 */

#ifndef _EXT4_COMMON_H
#define _EXT4_COMMON_H

#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <linux/types.h>
#include <ext2fs/ext2fs.h>
#include <sys/stat.h>
#include "recovered_intervals.h"
#include "fs_capabilities.h"
#include "journal_index.h"

#define RECOVER_DIR "./RECOVER"
#define VERSION "0.5"
#define EXT4_EXT_MAGIC 0xF30A
#define CHECKPOINT_FILE ".ext4recover_checkpoint.json"
#define MAX_FILENAME_MAP 100000

/* EXT4 inode flags */
#ifndef EXT4_EXTENTS_FL
#define EXT4_EXTENTS_FL 0x00080000
#endif

/* Recovery modes */
#define RECOVER_MODE_NORMAL     0x01
#define RECOVER_MODE_ORPHAN     0x02
#define RECOVER_MODE_JOURNAL    0x04
#define RECOVER_MODE_AGGRESSIVE 0x08
#define RECOVER_MODE_TARGETED   0x10   /* C1: revoke-guided scan */
#define RECOVER_MODE_ALL        0xFF

/* Filename mapping entry */
struct filename_entry {
    __u32 inode;
    char name[256];
};

/* A4: output-name claim registry. First inode to ask for a basename
 * owns it; later inodes mapping to the same basename are diverted to
 * "<ino>_file" instead of being silently skipped / overwriting. */
struct name_claim {
    __u32 ino;
    char name[256];
};

/* Global context structure */
struct recover_context {
    ext2_filsys fs;
    int device_fd;
    int recover_fd;
    char *device_name;
    char recover_dir[256];
    int mode;
    int verbose;
    int trim_zeros;
    
    /* Statistics */
    unsigned long files_recovered;
    unsigned long files_failed;
    unsigned long orphan_recovered;
    unsigned long journal_recovered;
    unsigned long aggressive_recovered;
    
    /* Block info */
    ssize_t blocksize;
    blk64_t total_blocks;
    
    /* Bigalloc/Cluster support */
    int cluster_ratio;          /* cluster_size / block_size */
    int log_cluster_size;       /* s_log_cluster_size from superblock */
    int has_bigalloc;
    
    /* Journal info */
    int journal_fd;
    blk64_t journal_start;
    blk64_t journal_len;
    int has_journal;
    ext2_file_t journal_file;   /* B1: cached handle, opened once */
    struct journal_index *jindex; /* B2: fs_block -> journal copies */
    
    /* Filename mapping */
    struct filename_entry *filename_map;
    int filename_count;
    int filename_capacity;

    /* A4: output-name claims (collision diversion) */
    struct name_claim *name_claims;
    int claim_count;
    int claim_capacity;
    
    /* Already-recovered physical block intervals (cross-phase dedup) */
    struct recovered_intervals *recovered_extents;
    unsigned long dedup_skipped;          /* extents skipped due to dedup */
    unsigned long dedup_skipped_blocks;   /* blocks skipped due to dedup */
    
    /* Aggressive parallel scan options */
    int use_parallel;          /* 1 = opt-in via --parallel; default OFF (IO-bound) */
    int n_workers;             /* worker thread count (0 = auto) */
    
    /* On-disk format capabilities (Phase 0.1) */
    struct fs_capabilities caps;

    /* Checkpoint/resume */
    int resume_mode;
    blk64_t checkpoint_journal_offset;
    unsigned long checkpoint_files_recovered;
    char checkpoint_file[256];
};

/* Extent path structure */
struct extent_path {
    char *buf;
    int entries;
    int max_entries;
    int left;
    int visit_num;
    int flags;
    blk64_t end_blk;
    void *curr;
};

/* Extent handle structure */
struct ext2_extent_handle {
    errcode_t magic;
    ext2_filsys fs;
    ext2_ino_t ino;
    struct ext2_inode *inode;
    struct ext2_inode inodebuf;
    int type;
    int level;
    int max_depth;
    int max_paths;
    struct extent_path *path;
};

/* Recovered file info */
struct recovered_file {
    __u32 inode_num;
    char filename[256];
    off_t size;
    time_t mtime;
};

/* Function prototypes - main recovery */
int recover_from_extent_tree(struct recover_context *ctx, __u32 ino,
                             struct ext2_inode *inode);

/* Function prototypes - orphan list */
int recover_orphan_list(struct recover_context *ctx);
int scan_orphan_chain(struct recover_context *ctx, __u32 start_ino);

/* Function prototypes - journal */
int init_journal(struct recover_context *ctx);
int recover_from_journal(struct recover_context *ctx);
int parse_journal_block(struct recover_context *ctx, blk64_t blk);
void close_journal(struct recover_context *ctx);

/* Function prototypes - aggressive scan */
int aggressive_scan(struct recover_context *ctx);
int scan_for_extent_headers(struct recover_context *ctx, blk64_t start,
                            blk64_t end);

/* Function prototypes - extent validation */
int validate_extent_header(struct ext3_extent_header *eh, int blocksize);
int calculate_max_entries(int blocksize, int is_leaf);
int deduplicate_extents(struct ext3_extent *extents, int count);

/* Function prototypes - utilities */
int is_on_device(const char *path, const char *dev);
int recover_block_to_file(int devfd, int inofd, __le32 block, __le16 len,
                         __u64 start, ssize_t blocksize, struct recover_context *ctx);
void print_stats(struct recover_context *ctx);
int create_recovery_file(struct recover_context *ctx, __u32 ino, int *fd_out);

/* Function prototypes - bigalloc support */
void init_cluster_info(struct recover_context *ctx);

/* Function prototypes - filename mapping */
int init_filename_map(struct recover_context *ctx);
int add_filename_mapping(struct recover_context *ctx, __u32 inode, const char *name);
const char* get_filename_for_inode(struct recover_context *ctx, __u32 inode);
void parse_directory_blocks(struct recover_context *ctx);
void free_filename_map(struct recover_context *ctx);

/* A4: resolve final output basename for an inode, with collision
 * diversion. Fills buf and returns it. Idempotent per inode. */
const char *resolve_output_name(struct recover_context *ctx, __u32 ino,
                                char *buf, size_t bufsize);

/* Function prototypes - C9 manifest */
void write_manifest(struct recover_context *ctx);

/* Function prototypes - C8 indirect block recovery */
int recover_indirect_file(struct recover_context *ctx, __u32 ino,
                          struct ext2_inode_large *jinode);

/* Function prototypes - C4 orphan file */
int recover_orphan_file(struct recover_context *ctx);

/* Function prototypes - C6 inline data */
int recover_inline_data(struct recover_context *ctx, __u32 ino,
                        struct ext2_inode_large *jinode);

/* Function prototypes - C1 revoke targeted scan */
int recover_from_revoke(struct recover_context *ctx);

/* Function prototypes - C7 ghost dirent scan */
void scan_ghost_dirents(struct recover_context *ctx);

/* Function prototypes - checkpoint/resume */
int save_checkpoint(struct recover_context *ctx);
int load_checkpoint(struct recover_context *ctx);
void clear_checkpoint(struct recover_context *ctx);

/* Function prototypes - journal directory scanning */
void parse_journal_directory_blocks(struct recover_context *ctx);

/* Logging macros */
#define LOG_ERROR(fmt, ...)     fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)     fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)

#define LOG_INFO(fmt, ...)     fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)

#define LOG_DEBUG(ctx, fmt, ...)     do { if ((ctx)->verbose) fprintf(stdout, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)

/* Error handling */
#define RETURN_IF_ERROR(retval, msg)     do { if (retval) { LOG_ERROR("%s: %d", msg, retval); return retval; } } while(0)

#endif /* _EXT4_COMMON_H */
