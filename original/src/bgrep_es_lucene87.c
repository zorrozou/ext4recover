/*
 * Copyright 2020 Tencent Inc.  All rights reserved.
 * Author: curuwang@tencent.com
*/
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#define BLOCK_SIZE 4096
#define MAX_VALID_BLOCKS 131072
#define RECOVER_DIR "./RECOVER"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file\n", argv[0]);
        exit(1);
    }
    printf("creating recover directory '%s'\n", RECOVER_DIR);
    if (mkdir(RECOVER_DIR, 0700) < 0) {
        perror("failed to create recover dir:");
        return 1;
    }
    // fdx
    char *match_fdx_start = "\x3f\xd7\x6c\x17\x1dLucene85FieldsIndexMeta";
    int match_fdx_len = 28;
    // fdt
    char *match_fdt1_start = "\x3f\xd7\x6c\x17\x1cLucene87StoredFieldsFastData";
    int match_fdt1_len = 33;
    char *match_fdt2_start = "\x3f\xd7\x6c\x17\x1cLucene87StoredFieldsHighData";
    int match_fdt2_len = 33;
    char *match_fdt3_start = "\x3f\xd7\x6c\x17\x1cLucene87StoredFieldsZstandard";
    int match_fdt3_len = 34;
    // cfs
    char *match_cfs_start = "\x3f\xd7\x6c\x17\x14Lucene50CompoundData";
    int match_cfs_len = 25;
    // cfe
    char *match_cfe_start = "\x3f\xd7\x6c\x17\x17Lucene50CompoundEntries";
    int match_cfe_len = 28;
    // fnm
    char *match_fnm_start = "\x3f\xd7\x6c\x17\x12Lucene60FieldInfos";
    int match_fnm_len = 23;
    // si
    char *match_si_start = "\x3f\xd7\x6c\x17\x13Lucene86SegmentInfo";
    int match_si_len = 24;

    char *match_end = "\xc0\x28\x93\xe8\x00\x00\x00\x00";
    char match_end_len = 8;
    char *f = argv[1];
    printf("recovering from %s\n", f);

    int fd = open(f, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    size_t nread, blk_number=0;
    char *buf;
    char wfilename[100];
    int wfd = 0;
    char *p;
    char *pend;
    int started = 0;
    size_t nblocks = 0;
    buf = malloc(BLOCK_SIZE);
    while (nread = read(fd, buf, BLOCK_SIZE)) {
        p = buf;
        if (!started) {
            if(((p = memmem(buf, nread, match_fdt1_start, match_fdt1_len)) != NULL) ||
              ((p = memmem(buf, nread, match_fdt2_start, match_fdt2_len)) != NULL) ||
              ((p = memmem(buf, nread, match_fdt3_start, match_fdt3_len)) != NULL)){
                sprintf(wfilename, "%s/%lu.fdt", RECOVER_DIR, blk_number);
            }
            /*
            else if ((p = memmem(buf, nread, match_cfs_start, match_cfs_len)) == buf) {
                sprintf(wfilename, "%s/%lu.cfs", RECOVER_DIR, blk_number);
            }
            else if ((p = memmem(buf, nread, match_cfe_start, match_cfe_len)) == buf) {
                sprintf(wfilename, "%s/%lu.cfe", RECOVER_DIR, blk_number);
            }
            else if( (p=memmem(buf, nread, match_fdx_start, match_fdx_len)) == buf){
                sprintf(wfilename, "%s/%lu.fdx", RECOVER_DIR, blk_number);
            }else if( (p=memmem(buf, nread, match_fnm_start, match_fnm_len)) == buf){
                sprintf(wfilename, "%s/%lu.fnm", RECOVER_DIR, blk_number);
            }else if( (p=memmem(buf, nread, match_si_start, match_si_len)) == buf){
                sprintf(wfilename, "%s/%lu.si", RECOVER_DIR, blk_number);
            }
            */

            if (p != NULL) {
                started = 1;
                nread -= p - buf; //buf will start at p
                buf = p;
                nblocks = 0;
                printf("found match file begin at block:%-10lu\n", blk_number);
                printf("dump file to %s... begin\n", wfilename);
                fflush(stdout);
                wfd = open(wfilename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (wfd < 0) {
                    perror("open");
                    return 1;
                }
            }
        }

        if (started) {
            pend = memmem(buf, nread, match_end, match_end_len);
            // found end str
            if (pend != NULL) {
                pend = pend + match_end_len + 8;
                write(wfd, buf, pend - buf);
                printf("found match end at block:%-10lu\n", blk_number);
                close(wfd);
                started = 0;
            } else if (nblocks > MAX_VALID_BLOCKS) {
                write(wfd, buf, nread);
                printf("reach MAX_VALID_BLOCKS at block:%-10lu\n", blk_number);
                close(wfd);
                started = 0;
            } else {
                write(wfd, buf, nread);
                nblocks += 1;
            }
        }
        blk_number += 1;
    }
    close(fd);
}

