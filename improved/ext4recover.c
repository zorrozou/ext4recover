/*
 * History:
 * 2020-03-01\t- Creation by zorrozou
 * 2020-03-08\t- beta 0.1
 * 2020-08-05   - beta 0.2 split from e2fsprogs code by curuwang
 */

#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/types.h>
#include <ext2fs/ext2fs.h>
#include <sys/stat.h>

#define RECOVER_DIR "./RECOVER"
#define VERSION "0.2b"

static const char *program_name = "ext4recover";
static char *device_name = NULL;
static int flag = 0;
static __u32 icount;
static struct ext3_extent_header *eh;
static struct ext3_extent_idx *ei;
static struct ext3_extent *ee;
static ssize_t blocksize;
static int recover_fd, device_fd;

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

static int is_inode_extent_clear(struct ext2_inode *inode) {
    eh = (struct ext3_extent_header *) inode->i_block;
    ei = (struct ext3_extent_idx *) eh + 1;
    ee = (struct ext3_extent *) eh + 2;
    blk64_t blk;
    blk = ext2fs_le32_to_cpu(ei->ei_leaf) +
          ((__u64) ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32);
    if (ei->ei_leaf > 1 && LINUX_S_ISREG(inode->i_mode) && inode->i_links_count == 0) {
        fprintf(stdout, "inode: %u extent_block: %llu\
", icount, blk);
        // not clear
        return 0;
    }

    return 1;
}

static int recover_block_to_file(int devfd, int inofd, __le32 block, __le16 len, __u64 start) {
    off_t offset_dev, offset_ino;
    ssize_t got, ret;
    char buf[blocksize];
    int i;

    offset_dev = lseek(devfd, (off_t)start * blocksize, SEEK_SET);
    if (offset_dev < 0) {
        perror("lseek(devfd)");
        return 0;
    }

    offset_ino = lseek(inofd, (off_t)block * blocksize, SEEK_SET);
    if (offset_ino < 0) {
        perror("lseek(inofd)");
        return 0;
    }
    /* prevent unused-variable warning when assertions are off */
    (void)offset_dev;
    (void)offset_ino;

    for (i = 0; i < len; i++) {
        ssize_t off = 0;
        /* read one full filesystem block from the source device */
        while (off < blocksize) {
            got = read(devfd, buf + off, blocksize - off);
            if (got < 0) {
                /* real I/O error -- bubble up so the caller can skip this
                 * inode and continue with the next one (the original code
                 * lseek+continue'd here, which left `off` unreset and
                 * could write garbage into the output file). */
                perror("read(devfd)");
                return 0;
            }
            if (got == 0) {
                /* EOF before we got a full block: the device is shorter
                 * than the inode claims, or extent points beyond device.
                 * Original code looped forever here. */
                fprintf(stderr,
                        "ext4recover: short read on device, "
                        "wanted %zd more bytes at extent block %llu\
",
                        (ssize_t)blocksize - off, (unsigned long long)start);
                return 0;
            }
            off += got;
        }
        /* write that block to the output file */
        ssize_t written = 0;
        while (written < blocksize) {
            ret = write(inofd, buf + written, blocksize - written);
            if (ret < 0) {
                perror("write(inofd)");
                return 0;
            }
            if (ret == 0) {
                fprintf(stderr,
                        "ext4recover: write returned 0 (output disk full?)\
");
                return 0;
            }
            written += ret;
        }
    }

    return 1;
}

static int dump_dir_extent(struct ext3_extent_header *eh) {
    struct ext3_extent *ee;
    int i;
    __le32 ee_block;
    __le16 ee_len;
    __u64 ee_start;
    int retval;

    ee = EXT_FIRST_EXTENT(eh);
//printf("eh->eh_entries: %d\
", eh->eh_entries);
//printf("eh->eh_max: %d\
", eh->eh_max);
    if (ext2fs_le16_to_cpu(eh->eh_entries) > 340) {
        return 1;
    } else if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT3_EXT_MAGIC) {
        return 1;
    } else if (ext2fs_le16_to_cpu(eh->eh_max) != 340) {
        return 1;
    }
    for (i = 1; i < eh->eh_entries + 1; i++) {
        ee_block = ext2fs_le32_to_cpu(ee->ee_block);
        ee_len   = ext2fs_le16_to_cpu(ee->ee_len);
        ee_start = (((__u64) ext2fs_le16_to_cpu(ee->ee_start_hi) << 32) +
                    (__u64) ext2fs_le32_to_cpu(ee->ee_start));
        printf("%u %u %u %llu\
", icount, ee_block, ee_len, ee_start);
        fflush(stdout);

        retval = recover_block_to_file(device_fd, recover_fd, ee_block, ee_len, ee_start);
        if (!retval) {
            fprintf(stderr, "recover_block_to_file()\
");
            return 0;
        }

        ee++;
    }
    return 1;
}

static int extent_tree_travel(ext2_extent_handle_t handle, struct ext3_extent_header *eh) {
    struct ext3_extent_header *next;
    struct ext3_extent_idx *ei;
    int i, retval;
    char *buf = NULL;
    blk64_t blk;
    int ok = 1;

    if (eh->eh_depth == 0) {
        retval = dump_dir_extent(eh);
        if (!retval) {
            fprintf(stderr, "dump_dir_extent()\
");
            return 0;
        }
    } else if (eh->eh_depth <= 4) {
        flag = 1;
        for (i = 1; i < eh->eh_entries + 1; i++) {
            retval = ext2fs_get_mem(blocksize, &buf);
            if (retval) {
                /* allocation failed: nothing to free, just abort */
                return 0;
            }
            memset(buf, 0, blocksize);
            ei = EXT_FIRST_INDEX(eh) + i - 1;
            blk = ext2fs_le32_to_cpu(ei->ei_leaf) +
                  ((__u64) ext2fs_le16_to_cpu(ei->ei_leaf_hi) << 32);
            retval = io_channel_read_blk64(handle->fs->io,
                                           blk, 1, buf);
            if (retval) {
                /* I/O error reading the child extent block: previously
                 * `return retval` -- but the caller checks `if (!retval)`
                 * for failure, so a non-zero errcode was being reported
                 * as success. Free buf and return 0 (= failure). */
                ext2fs_free_mem(&buf);
                ok = 0;
                break;
            }
            next = (struct ext3_extent_header *) buf;

            /* The child block may have been overwritten/reallocated to
             * something else after the file was deleted. Validate its
             * header before recursing -- reading it as an extent_header
             * blindly can crash on garbage (huge eh_entries, bad depth). */
            if (ext2fs_le16_to_cpu(next->eh_magic) != EXT3_EXT_MAGIC ||
                ext2fs_le16_to_cpu(next->eh_entries) >
                ext2fs_le16_to_cpu(next->eh_max) ||
                ext2fs_le16_to_cpu(next->eh_max) > 1024 ||
                next->eh_depth >= 5) {
                /* not a valid extent header anymore; skip this branch
                 * but keep going on siblings */
                ext2fs_free_mem(&buf);
                continue;
            }

            retval = extent_tree_travel(handle, next);
            ext2fs_free_mem(&buf);
            if (!retval) {
                fprintf(stderr, "extent_tree_travel()\
");
                /* keep going on remaining siblings -- this matches the
                 * pre-existing behavior of just printing and continuing */
            }
        }
    } else {
        /* depth > 4 means a corrupted or unsupported tree; bail out */
        return 1;
    }
    return ok;
}

static int prase_ino_extent(ext2_extent_handle_t handle) {
    int i, retval;
    struct ext3_extent_idx *ix;
    struct ext3_extent_header *next;
    char *buf = NULL;
    blk64_t blk;
    struct ext3_extent_header *eh;
    int ok = 1;

    eh = (struct ext3_extent_header *) handle->inode->i_block;

    retval = ext2fs_get_mem(blocksize, &buf);
    if (retval)
        return 0;
    memset(buf, 0, blocksize);

    for (i = 1; i <= 4; i++) {
        ix = EXT_FIRST_INDEX(eh) + i - 1;

        blk = ext2fs_le32_to_cpu(ix->ei_leaf) +
              ((__u64) ext2fs_le16_to_cpu(ix->ei_leaf_hi) << 32);
        retval = io_channel_read_blk64(handle->fs->io,
                                       blk, 1, buf);
        if (retval) {
            /* I/O error reading the index block: previously `return retval`
             * leaked buf and reported failure as success (caller checks
             * `if (!retval)`). */
            ok = 0;
            break;
        }

        next = (struct ext3_extent_header *) buf;

        /* sanity-check before recursing: the block may have been reused
         * for unrelated data after the original file was deleted */
        if (ext2fs_le16_to_cpu(next->eh_magic) != EXT3_EXT_MAGIC ||
            ext2fs_le16_to_cpu(next->eh_entries) >
            ext2fs_le16_to_cpu(next->eh_max) ||
            ext2fs_le16_to_cpu(next->eh_max) > 1024 ||
            next->eh_depth >= 5) {
            /* not a valid sub-tree; this whole inode's residue is gone */
            ok = 0;
            break;
        }

        retval = extent_tree_travel(handle, next);
        if (!retval) {
            fprintf(stderr, "extent_tree_travel()\
");
            ok = 0;
            break;
        }
        /* for last extent */
        if (next->eh_entries < 340) {
            break;
        }
    }
    ext2fs_free_mem(&buf);
    return ok;
}

static int is_on_device(const char *path, const char *dev) {
    struct stat stat1, stat2;
    int ret;
    ret = stat(path, &stat1);
    if (ret < 0) {
        perror("stat:");
        return -1;
    }
    ret = stat(dev, &stat2);
    if (ret < 0) {
        perror("stat:");
        return -1;
    }
    if (((stat2.st_mode & S_IFMT) == S_IFBLK) && (stat1.st_dev == stat2.st_rdev)) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    errcode_t retval;
    blk64_t use_superblock = 0;
    ext2_filsys fs;
    int use_blocksize = 0;
    int flags;
    __u32 imax;
    struct ext2_inode inode;
    ext2_extent_handle_t handle;
    char filename[BUFSIZ];

    if (argc != 2) {
        fprintf(stderr, "Usage: %s /dev/xxx\
", argv[0]);
        fprintf(stderr, "Recover deleted files using remaining extent info\
");
        fprintf(stderr, "version: %s\
\
", VERSION);
        fprintf(stderr, "eg:\
\\t%s /dev/vdb1\
", argv[0]);
        exit(1);
    }

    device_name = argv[1];
    flags = EXT2_FLAG_JOURNAL_DEV_OK | EXT2_FLAG_SOFTSUPP_FEATURES |
            EXT2_FLAG_64BITS;
    retval = ext2fs_open(device_name, flags, use_superblock,
                         use_blocksize, unix_io_manager, &fs);

    if (retval) {
        com_err(program_name, retval, "while trying to open %s",
                device_name);
        printf("%s", "Couldn't find valid filesystem superblock.\
");
        exit(retval);
    }
    fs->default_bitmap_type = EXT2FS_BMAP64_RBTREE;

    blocksize = fs->blocksize;

    device_fd = open(argv[1], O_RDONLY);
    if (device_fd < 0) {
        perror("open(device)");
        exit(1);
    }

    retval = mkdir(RECOVER_DIR, 0750);
    if (retval < 0 && errno != EEXIST) {
        perror("mkdir()");
        exit(1);
    }
    if (is_on_device(RECOVER_DIR, device_name) == 1) {
        fprintf(stderr, "DANGER: recover dir '%s' is on target device '%s', aborted!\
",
                RECOVER_DIR, device_name);
        exit(1);
    }
    imax = fs->super->s_inodes_count;
    for (icount = 3; icount < imax + 1; icount++) {
        flag = 0;

        retval = ext2fs_read_inode(fs, icount, &inode);
        if (retval) {
            com_err(program_name, retval, "%s",
                    "while reading inode");
            continue;
        }
        if (is_inode_extent_clear(&inode)) {
            continue;
        }

        snprintf(filename, BUFSIZ, "%s/%d_file", RECOVER_DIR, icount);

        recover_fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_LARGEFILE, 0640);
        if (recover_fd < 0) {
            perror("open(inode)");
            continue;
        }

        retval = ext2fs_extent_open(fs, icount, &handle);
        if (retval) {
            close(recover_fd);
            continue;
        }

        retval = prase_ino_extent(handle);
        if (!retval) {
            /* Per-inode failure: skip this one and keep recovering the
             * rest. The original code did exit(1) here, which threw away
             * every later inode the moment one bad one was hit. */
            fprintf(stderr,
                    "ext4recover: skipping inode %u (extent walk failed)\
",
                    icount);
        }
        if (flag) {
            fflush(stdout);
        }

        ext2fs_extent_free(handle);   /* matches ext2fs_extent_open above */
        close(recover_fd);
    }

    close(device_fd);

    fprintf(stderr, "Recover success!\
");

    exit(0);
}


