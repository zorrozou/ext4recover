# Design — Cross-phase Deduplication via Interval Tree

Status: ✅ implemented and verified on real disk (Phase 2).

## The problem

`ext4recover` walks deleted-inode metadata and dumps the underlying
physical extents. Multiple recovery phases can independently
discover the *same* physical block region:

* **Normal** mode walks deleted inodes and emits dumps as
  `<inode>_file` (or filename, if a directory-block scan recovered the
  name).
* **Journal** mode reads jbd2 descriptor blocks for older inode
  copies, which point at *the same physical leaves*. Then it dumps as
  `<filename>` (the journal copy often still has its dir-block name).
* **Aggressive** mode magic-scans the entire device for `0xf30a`
  headers. The same leaf physical block can be hit:
  * once at its original inode-table position,
  * once via its journal-replicated copy,
  * occasionally via free-block residue in the last allocator pool,

  and each hit creates a brand-new `aggressive_<block>` file.

T1 demonstrated this concretely: a single 2 GB file produced 1 file
from journal phase **and** 1 separate `aggressive_<block>` file from
the aggressive phase — both byte-identical. T-DEDUP-2 showed
aggressive alone produced 2 redundant `aggressive_*` files for the
same content out of 5 leaf-header hits.

There are five overlap categories worth distinguishing:

1. **Identity** — same physical extents dumped twice (most common).
2. **Subset** — older journal version's extents are a strict subset
   of the latest one (or vice versa). Want to keep the larger one.
3. **Cross-phase identity** — `<inode>_file` from normal phase and
   `aggressive_<block>` from aggressive phase are the same file
   under different names.
4. **Leaf fragmentation** — depth>0 file currently produces N
   `aggressive_<leaf>` files, one per leaf, instead of being
   reassembled. Not solved here (Phase 4).
5. **Different inode reusing same physical block after free** —
   newer file overwrote the block, original content lost. Dedup
   correctly does NOT collapse these (their physical extents differ
   by definition, since the new write changed `ee_start`).

This module solves (1), (2), (3); flags (4) for Phase 4; is
orthogonal to (5).

## Data structure

```c
struct recovered_intervals {
    uint64_t *starts;     /* physical block start, sorted ascending */
    uint64_t *lens;       /* length in blocks */
    int count;
    int capacity;
    pthread_rwlock_t lock; /* unused in single-threaded build, ready for Phase 3 */
};
```

Why a sorted array instead of a red-black tree?

* The N stays small in practice. ext4 merges adjacent extents
  aggressively, and our `intervals_add` does the same on insert.
  T-DEDUP-1 saw `Dedup intervals: 3 distinct extents, 153600 blocks`
  for a 600 MB file. T0a saw 6 distinct intervals for 524288 blocks
  (2 GB). Even pathological cases stay in the thousands range.
* Binary search in a flat array is ~3× faster than equivalent RB-tree
  walks for small N (cache locality).
* Implementation is ~170 lines vs ~600 for a real RB-tree, easier
  to review under time pressure.

Insert algorithm (`intervals_add`):

1. Binary-search `merge_lo` = leftmost index with `end ≥ s`.
2. Binary-search `merge_hi` = first index with `start > e` (strict;
   so adjacent intervals starting exactly at `e` get merged).
3. If `merge_hi > merge_lo`, fold all those intervals + the new one
   into a single `[min start, max end)`, `memmove` the tail down.
4. Else insert at `merge_lo`, growing the array if needed.

Worst-case insert is O(N) due to memmove, but each merge reduces
count by `merge_hi - merge_lo - 1`, so amortized cost stays low.

Query algorithm (`intervals_query`):

* Returns 0 (no overlap), 1 (fully contained), 2 (partial overlap).
* `find_first_ge(query_start)` = first interval whose `end > q_s`.
  At that index check whether the candidate fully contains
  `[q_s, q_s+len)` or just overlaps.

## Hook points

| Hook | File:Function | Behavior |
|------|---------------|----------|
| Pre-write check | `improved/ext4recover_v5.c::recover_block_to_file` | If `intervals_query` returns 1, skip the write entirely (treat as success). Increment `ctx->dedup_skipped` counters. |
| Post-write register | same function, after fwrite loop | `intervals_add` so future queries see this region as covered. |
| Aggressive leaf pre-pass | `improved/aggressive_scan_v5.c::recover_orphaned_extent_block` | Walk the leaf's extents *before* opening any output file; if every extent is fully contained, return without creating the file. Avoids zero-byte `aggressive_<block>` files. |
| Journal leaf pre-pass | `improved/journal_recovery_v5.c::recover_inode_from_journal` (depth=0 path) | Same idea — abandon temp file creation if everything is already covered. |

Each hook is at most a few extra lines; they are conditional on
`ctx->recovered_extents != NULL`, so behaviour with dedup disabled
falls back to the original logic.

## Concurrency

The RW lock is a no-op in the current single-threaded build but is
important for Phase 3 (parallel aggressive scan), where worker
threads will do `intervals_query` concurrently and a single writer
thread will do `intervals_add`. Read path uses `rdlock` for
nearly-zero blocking.

If Phase 3 measures contention, the next step is per-thread shards
that periodically merge into the global tree.

## Stats

`print_stats` outputs:

```
Dedup intervals: N distinct extents, M blocks total; X writes skipped (Y blocks)
```

* N = number of merged intervals (i.e. distinct contiguous regions
  on disk that the recovery has touched).
* M = total blocks in the union of those intervals (= sum of
  recovered file sizes / blocksize).
* X = how many `recover_block_to_file` calls were short-circuited
  by dedup.
* Y = how many blocks would have been re-written without dedup.

## Test evidence

* `tests/test_intervals.c` — 21 unit tests passing on insertion,
  containment, adjacent merge, bridging merge, no-op, large addresses.
* `logs/t0a_dedup.log` — T0a regression gate, dedup intervals = 6,
  matches `filefrag` output exactly.
* `logs/tdedup1.log` — `--normal --journal` 600 MB, single output file.
* `logs/tdedup2_v5_summary.log` — aggressive 500 GB, 5 hits → 3 dumps.
* `logs/t2_dedup.log` — unexpected positive interaction: small-file
  journal recovery improved 2/10 → 7/10 because dedup blocks a
  later/wrong jinode copy from overwriting the earlier-correct dump.
