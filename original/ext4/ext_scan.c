/*
 * scan ext4 extent for data recovery
 * by curu, 2020-03-03
 * no rights reserved, share if it helps.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ext2fs/ext2fs.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define BLOCK_SIZE 4096
#define VALID_DEPTH 5
#define VALID_ENTRY 1024

struct ext3_extent_header * read_ext_hdr_block(unsigned long blknum);
void print_ext_hdr(struct ext3_extent_header *hdr, int indent);
static char *dev;
static int dev_fd = -1; /* opened once and reused via pread */

void print_ext_idx(struct ext3_extent_idx *idx, int depth, int indent){
    struct ext3_extent_header * hdr;
    uint64_t block_num = ((uint64_t)idx->ei_leaf_hi << 32) | idx->ei_leaf;

    printf("%*s%s", indent, "", ">");
    printf("ei_block:%-10u ei_leaf:%-10u ei_leaf_hi:%-10u BLOCK_ADDR:%llu\n",
            idx->ei_block, idx->ei_leaf, idx->ei_leaf_hi, (unsigned long long)block_num);

    /* If parent header says there's depth > 0, the idx points to another extent header/block.
 *  *        We always try to read the block and validate it before recursively printing. */
    if (depth > 0){
        hdr = read_ext_hdr_block((unsigned long)block_num);
        if (hdr != NULL){
            /* Basic sanity checks to avoid interpreting garbage as headers and infinite loops */
            if (hdr->eh_magic == EXT3_EXT_MAGIC
                && hdr->eh_entries <= hdr->eh_max
                && hdr->eh_max < VALID_ENTRY
                && hdr->eh_depth < VALID_DEPTH) {
                print_ext_hdr(hdr, indent+1);
            } else {
                printf("%*s%s", indent+1, "", ">");
                printf("Invalid/Untrusted extent header at block:%llu (magic=%x entries=%u max=%u depth=%u)\n",
                        (unsigned long long)block_num, hdr->eh_magic, hdr->eh_entries, hdr->eh_max, hdr->eh_depth);
            }
            free(hdr);
        } else {
            printf("%*s%s", indent+1, "", ">");
            printf("Failed to read block %llu: %s\n", (unsigned long long)block_num, strerror(errno));
        }
    }
}

void print_ext(struct ext3_extent *ext, int indent){
    uint64_t leaf_block = ((uint64_t)ext->ee_start_hi << 32) | ext->ee_start;
    printf("%*s%s", indent, "", ">");
    printf("ee_block:%-10u ee_len:%-5u ee_start_hi:%-5u ee_start:%-10u LEAF_BLOCK_ADDR:%llu\n",
            ext->ee_block, ext->ee_len, ext->ee_start_hi,
            ext->ee_start, (unsigned long long)leaf_block);
}

void print_ext_hdr(struct ext3_extent_header *hdr, int indent){
    struct ext3_extent_idx* ext_idx;
    struct ext3_extent* ext;
    int i;

    printf("%*s%s", indent, "", ">");
    printf("eh_magic:%X eh_entries:%-5u eh_max:%-5u eh_depth:%-5u eh_generation:%u\n",
            hdr->eh_magic, hdr->eh_entries, hdr->eh_max,
            hdr->eh_depth, hdr->eh_generation);

    /* safety: ensure eh_entries doesn't exceed reasonable bounds */
    if (hdr->eh_entries > hdr->eh_max || hdr->eh_entries >= VALID_ENTRY) {
        printf("%*s%s Invalid eh_entries count (%u)\n", indent+1, "", ">", hdr->eh_entries);
        return;
    }

    /* entries area starts immediately after the header struct */
    char *base = (char*)hdr + sizeof(struct ext3_extent_header);

    for (i = 0; i < (int)hdr->eh_entries; i++){
        if(hdr->eh_depth > 0){
            ext_idx = (struct ext3_extent_idx*)(base + i * sizeof(struct ext3_extent_idx));
            print_ext_idx(ext_idx, hdr->eh_depth, indent+1);
        }else{
            ext = (struct ext3_extent*)(base + i * sizeof(struct ext3_extent));
            print_ext(ext, indent+1);
        }
    }
}

struct ext3_extent_header * read_ext_hdr_block(unsigned long blknum){
    ssize_t nread;
    struct ext3_extent_header *hdr;

    if (dev_fd < 0) {
        /* open if not already */
        dev_fd = open(dev, O_RDONLY);
        if (dev_fd < 0){
            perror("open");
            return NULL;
        }
    }

    hdr = malloc(BLOCK_SIZE);
    if (!hdr) return NULL;

    /* use pread so we don't disturb other reads that rely on file offset */
    nread = pread(dev_fd, hdr, BLOCK_SIZE, (off_t)blknum * BLOCK_SIZE);
    if (nread != BLOCK_SIZE){
        free(hdr);
        if (nread < 0) {
            /* errno already set */
            return NULL;
        } else {
            /* partial read (e.g., end of device) */
            errno = EIO;
            return NULL;
        }
    }

    return hdr;
}

int main(int argc, char **argv){
    ssize_t nread;
    unsigned long blk_number = 0;
    struct ext3_extent_header* ext_hdr;

    if ( argc < 2 ){
        fprintf(stderr, "Usage: %s /dev/xxxx\n", argv[0]);
        return 1;
    }
    dev = argv[1];

    /* open once for scanning main data (we will still use pread in read_ext_hdr_block) */
    int fd = open(dev, O_RDONLY);
    if (fd < 0){
        perror("open");
        return 1;
    }

    char buf[BLOCK_SIZE];
    while((nread = read(fd, buf, BLOCK_SIZE)) > 0){
        if ((size_t)nread < sizeof(struct ext3_extent_header)) {
            blk_number += 1;
            continue;
        }
        ext_hdr = (struct ext3_extent_header*)buf;
        if (ext_hdr->eh_magic == EXT3_EXT_MAGIC
                && ext_hdr->eh_entries <= ext_hdr->eh_max
                && ext_hdr->eh_max < VALID_ENTRY
                && ext_hdr->eh_depth < VALID_DEPTH){
            printf("Found extent header at block:%-10lu\n", blk_number);
            print_ext_hdr(ext_hdr, 0);
            printf("\n");
            fflush(stdout);
        }
        blk_number += 1;
    }

    if (dev_fd >= 0) close(dev_fd);
    close(fd);
    return 0;
}
