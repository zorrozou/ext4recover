/* unit test for recovered_intervals */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "recovered_intervals.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("[FAIL] %s\n", msg); fflush(stdout); exit(1); \
    } else { \
        printf("[OK]   %s\n", msg); fflush(stdout); \
    } \
} while(0)

int main(void) {
    struct recovered_intervals *ri;
    int rc = intervals_init(&ri);
    CHECK(rc == 0, "init");

    /* empty: query returns 0 */
    CHECK(intervals_query(ri, 100, 10) == 0, "empty query == 0");

    /* add [100, 200) */
    intervals_add(ri, 100, 100);
    int n; uint64_t tot;
    intervals_stats(ri, &n, &tot);
    CHECK(n == 1 && tot == 100, "add [100,200) -> 1 interval, 100 blocks");

    /* fully contained */
    CHECK(intervals_query(ri, 120, 30) == 1, "[120,150) ⊂ [100,200) -> 1");
    /* exact match */
    CHECK(intervals_query(ri, 100, 100) == 1, "[100,200) == [100,200) -> 1");
    /* boundary: starts at start */
    CHECK(intervals_query(ri, 100, 50) == 1, "[100,150) ⊂ [100,200) -> 1");
    /* boundary: ends at end */
    CHECK(intervals_query(ri, 150, 50) == 1, "[150,200) ⊂ [100,200) -> 1");
    /* no overlap left */
    CHECK(intervals_query(ri, 50, 40) == 0, "[50,90) no overlap -> 0");
    /* no overlap right */
    CHECK(intervals_query(ri, 250, 50) == 0, "[250,300) no overlap -> 0");
    /* partial overlap left */
    CHECK(intervals_query(ri, 80, 50) == 2, "[80,130) partial -> 2");
    /* partial overlap right */
    CHECK(intervals_query(ri, 180, 50) == 2, "[180,230) partial -> 2");
    /* superset */
    CHECK(intervals_query(ri, 50, 200) == 2, "[50,250) superset -> 2");

    /* add [300, 400), check 2 intervals */
    intervals_add(ri, 300, 100);
    intervals_stats(ri, &n, &tot);
    CHECK(n == 2 && tot == 200, "add [300,400) -> 2 intervals, 200 blocks");

    /* add [200, 300) -> should merge all into [100, 400) */
    intervals_add(ri, 200, 100);
    intervals_stats(ri, &n, &tot);
    CHECK(n == 1 && tot == 300, "add [200,300) merges all -> 1 interval, 300 blocks");

    /* add adjacent [400, 500) -> merges */
    intervals_add(ri, 400, 100);
    intervals_stats(ri, &n, &tot);
    CHECK(n == 1 && tot == 400, "add [400,500) adjacent merge");

    /* add disjoint [1000, 1100), then [600, 700), then [700, 1000) */
    intervals_add(ri, 1000, 100);
    intervals_add(ri,  600, 100);
    intervals_stats(ri, &n, &tot);
    CHECK(n == 3 && tot == 600, "3 disjoint ranges");

    intervals_add(ri,  700, 300);
    intervals_stats(ri, &n, &tot);
    printf("    [DEBUG] after add [700,1000): n=%d tot=%lu, contents:", n, (unsigned long)tot);
    for (int i = 0; i < ri->count; i++)
        printf(" [%lu,%lu)", (unsigned long)ri->starts[i], (unsigned long)(ri->starts[i]+ri->lens[i]));
    printf("\n");
    CHECK(n == 2 && tot == 900, "[700,1000) bridges -> 2 intervals, tot=900");
    /* query across merged region */
    CHECK(intervals_query(ri, 600, 500) == 1, "[600,1100) merged contains query");

    /* re-add a fully covered range, should not change */
    intervals_add(ri, 250, 50);
    intervals_stats(ri, &n, &tot);
    CHECK(n == 2 && tot == 900, "re-add covered range no-op");

    /* large add */
    intervals_add(ri, 100000000, 1000000);
    intervals_stats(ri, &n, &tot);
    CHECK(n == 3 && tot == 1000900, "large physical block range OK");

    intervals_free(ri);
    printf("\nALL TESTS PASSED\n");
    return 0;
}
