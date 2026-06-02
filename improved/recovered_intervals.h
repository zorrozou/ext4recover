/*
 * recovered_intervals.h - sorted/merged set of physical block intervals
 * used to deduplicate dump operations across recovery phases.
 */
#ifndef _RECOVERED_INTERVALS_H
#define _RECOVERED_INTERVALS_H

#include <stdint.h>
#include <pthread.h>

struct recovered_intervals {
    uint64_t *starts;
    uint64_t *lens;
    int count;
    int capacity;
    pthread_rwlock_t lock;
};

int  intervals_init(struct recovered_intervals **out);
void intervals_free(struct recovered_intervals *ri);

/* 0 = no overlap, 1 = fully contained, 2 = partial overlap */
int  intervals_query(struct recovered_intervals *ri, uint64_t s, uint64_t len);
int  intervals_add  (struct recovered_intervals *ri, uint64_t s, uint64_t len);
void intervals_stats(struct recovered_intervals *ri,
                     int *out_count, uint64_t *out_total_blocks);

#endif
