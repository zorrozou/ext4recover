/*
 * ext4_common.h - Common definitions for ext4recover
 * 
 * Improved by ext4recover enhancement project
 * Based on original work by zorrozou and curuwang
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

#define RECOVER_DIR "./RECOVER"
#define VERSION "0.4"
#define EXT4_EXT_MAGIC 0xF30A

/* EXT4 inode flags */
#ifndef EXT4_EXTENTS_FL
#define EXT4_EXTENTS_FL 0x00080000
#endif

/* Recovery modes */
#define RECOVER_MODE_NORMAL     0x01  /* Original multi-level extent */
#define RECOVER_MODE_ORPHAN     0x02  /* Orphan list priority */
#define RECOVER_MODE_JOURNAL    0x04  /* Journal parsing */
#define RECOVER_MODE_AGGRESSIVE 0x08  /* Full-disk scan */
#define RECOVER_MODE_ALL        0xFF  /* All strategies */

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
    
    /* Journal info */
    int journal_fd;
    blk64_t journal_start;
    blk64_t journal_len;
    int has_journal;
};

/* Extent path structure (from original) */
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

/* Extent handle structure (from e2fsprogs internals) */
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
    int blocks_recovered;
    time_t timestamp;
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
                         __u64 start, ssize_t blocksize);
void print_stats(struct recover_context *ctx);
int create_recovery_file(struct recover_context *ctx, __u32 ino, int *fd_out);

/* Logging macros */
#define LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)

#define LOG_DEBUG(ctx, fmt, ...) \
    do { if ((ctx)->verbose) fprintf(stdout, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)

/* Error handling */
#define RETURN_IF_ERROR(retval, msg) \
    do { if (retval) { LOG_ERROR("%s: %d", msg, retval); return retval; } } while(0)

#endif /* _EXT4_COMMON_H */
