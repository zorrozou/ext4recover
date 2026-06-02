/*
 * recovered_intervals.c - Track already-recovered physical block ranges
 *
 * Used to deduplicate dump operations across normal/journal/aggressive
 * recovery phases. Maintains a sorted, merged list of [start, start+len)
 * physical block intervals.
 *
 * Design:
 *  - Sorted array of (start, len) pairs, merged on insert.
 *  - Binary search for containment / overlap queries.
 *  - Lock predeclared but not used in this single-threaded version;
 *    will be promoted to RW lock when aggressive scan is parallelized.
 */

#include "ext4_common_v5.h"
#include "recovered_intervals.h"
#include <pthread.h>

#define INITIAL_CAPACITY 1024

int intervals_init(struct recovered_intervals **out)
{
    struct recovered_intervals *ri = calloc(1, sizeof(*ri));
    if (!ri) return -1;
    ri->capacity = INITIAL_CAPACITY;
    ri->starts = calloc(ri->capacity, sizeof(uint64_t));
    ri->lens   = calloc(ri->capacity, sizeof(uint64_t));
    if (!ri->starts || !ri->lens) {
        free(ri->starts); free(ri->lens); free(ri);
        return -1;
    }
    pthread_rwlock_init(&ri->lock, NULL);
    *out = ri;
    return 0;
}

void intervals_free(struct recovered_intervals *ri)
{
    if (!ri) return;
    pthread_rwlock_destroy(&ri->lock);
    free(ri->starts);
    free(ri->lens);
    free(ri);
}

/* Find index of first interval whose end (start+len) > query_start.
 * Returns count if not found. Caller must hold (read)lock. */
static int find_first_ge(struct recovered_intervals *ri, uint64_t query_start)
{
    int lo = 0, hi = ri->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ri->starts[mid] + ri->lens[mid] > query_start)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/*
 * Query: how does [s, s+len) relate to the recorded set?
 *   0 = no overlap (caller should fully dump)
 *   1 = fully contained (caller should skip entirely)
 *   2 = partial overlap (caller may dump entire or refine; first version
 *       chooses to still dump and add — simpler, idempotent at byte level
 *       since same physical extent always yields same data).
 */
int intervals_query(struct recovered_intervals *ri, uint64_t s, uint64_t len)
{
    if (!ri || len == 0) return 0;
    pthread_rwlock_rdlock(&ri->lock);
    int idx = find_first_ge(ri, s);
    int rc = 0;
    if (idx < ri->count) {
        uint64_t i_s = ri->starts[idx];
        uint64_t i_e = i_s + ri->lens[idx];
        uint64_t q_e = s + len;
        if (i_s <= s && i_e >= q_e) {
            rc = 1; /* fully contained */
        } else if (i_s < q_e) {
            rc = 2; /* overlap but not contained */
        }
    }
    pthread_rwlock_unlock(&ri->lock);
    return rc;
}

/*
 * Insert [s, s+len) into the set, merging adjacent/overlapping intervals.
 * Returns 0 on success.
 */
int intervals_add(struct recovered_intervals *ri, uint64_t s, uint64_t len)
{
    if (!ri || len == 0) return 0;
    pthread_rwlock_wrlock(&ri->lock);

    /* Find leftmost interval that may merge with [s, s+len).
     * That is, first index where end >= s. */
    int lo = 0, hi = ri->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ri->starts[mid] + ri->lens[mid] >= s)
            hi = mid;
        else
            lo = mid + 1;
    }
    int merge_lo = lo;

    /* Find rightmost interval that may merge: first index where start > s+len.
     * Note: use > (strict) so an interval starting exactly at s+len IS merged
     * (adjacent intervals). */
    uint64_t e = s + len;
    lo = merge_lo; hi = ri->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ri->starts[mid] > e)
            hi = mid;
        else
            lo = mid + 1;
    }
    int merge_hi = lo; /* exclusive */

    if (merge_hi > merge_lo) {
        /* Merge with existing */
        uint64_t new_s = s;
        uint64_t new_e = e;
        if (ri->starts[merge_lo] < new_s) new_s = ri->starts[merge_lo];
        uint64_t last_e = ri->starts[merge_hi - 1] + ri->lens[merge_hi - 1];
        if (last_e > new_e) new_e = last_e;
        /* Replace [merge_lo, merge_hi) with single [new_s, new_e) */
        ri->starts[merge_lo] = new_s;
        ri->lens[merge_lo]   = new_e - new_s;
        int gap = (merge_hi - merge_lo) - 1;
        if (gap > 0) {
            int tail = ri->count - merge_hi;
            if (tail > 0) {
                memmove(&ri->starts[merge_lo + 1], &ri->starts[merge_hi], tail * sizeof(uint64_t));
                memmove(&ri->lens[merge_lo + 1],   &ri->lens[merge_hi],   tail * sizeof(uint64_t));
            }
            ri->count -= gap;
        }
    } else {
        /* No merge: insert at merge_lo */
        if (ri->count >= ri->capacity) {
            int new_cap = ri->capacity * 2;
            uint64_t *ns = realloc(ri->starts, new_cap * sizeof(uint64_t));
            uint64_t *nl = realloc(ri->lens,   new_cap * sizeof(uint64_t));
            if (!ns || !nl) {
                /* On OOM, keep state consistent and refuse insert.
                 * Worst case: dedup degrades but correctness is preserved. */
                if (ns) ri->starts = ns;
                if (nl) ri->lens = nl;
                pthread_rwlock_unlock(&ri->lock);
                return -1;
            }
            ri->starts = ns; ri->lens = nl;
            ri->capacity = new_cap;
        }
        int tail = ri->count - merge_lo;
        if (tail > 0) {
            memmove(&ri->starts[merge_lo + 1], &ri->starts[merge_lo], tail * sizeof(uint64_t));
            memmove(&ri->lens[merge_lo + 1],   &ri->lens[merge_lo],   tail * sizeof(uint64_t));
        }
        ri->starts[merge_lo] = s;
        ri->lens[merge_lo]   = len;
        ri->count++;
    }

    pthread_rwlock_unlock(&ri->lock);
    return 0;
}

void intervals_stats(struct recovered_intervals *ri,
                     int *out_count, uint64_t *out_total_blocks)
{
    if (!ri) { if (out_count) *out_count = 0;
               if (out_total_blocks) *out_total_blocks = 0;
               return; }
    pthread_rwlock_rdlock(&ri->lock);
    if (out_count) *out_count = ri->count;
    if (out_total_blocks) {
        uint64_t s = 0;
        for (int i = 0; i < ri->count; i++) s += ri->lens[i];
        *out_total_blocks = s;
    }
    pthread_rwlock_unlock(&ri->lock);
}
