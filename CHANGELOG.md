# CHANGELOG

Format: each entry includes the bug, root cause (with kernel/source
evidence), the fix, and the real-disk evidence proving the fix works.

---

## [unreleased / dedup_v1] — 2026-06-02

### Phase 2 ✅ — Cross-phase interval-tree deduplication

**Problem.** The aggressive scan reported a "found N orphaned extent
blocks" counter but had no awareness of overlap with what the normal
or journal phases already wrote. On real disks this caused:

* Journal phase first dumps `bigfile` (correct).
* Aggressive phase then re-scans the same physical blocks via
  the leaf-on-disk + the journal-replicated leaf and dumps the file
  again as `aggressive_<block>` — sometimes multiple times because
  ext4 writes the same leaf to inode-table, journal, and free-block
  positions. T1 showed this concretely on a 2 GB file.

There is also no relationship between an `aggressive_<block>` file and
the inode-named recovery from earlier phases — users cannot tell that
they are duplicates.

**Solution.** New module `recovered_intervals.{c,h}`:

* Sorted/merged set of physical block intervals.
* Three operations: `intervals_query(start, len)` (0 = no overlap,
  1 = fully contained, 2 = partial), `intervals_add(start, len)`
  (insert with adjacent + bridging merge), `intervals_stats`.
* Backed by sorted arrays + binary search; O(log N) query, O(N) worst
  case insert (memmove during merge), but in practice N stays small
  (extents merge fast in ext4).
* RW lock predeclared (`pthread_rwlock_t`) for use during Phase 3
  parallelization; not contended in current single-threaded code.

Hooked into three writers:

1. `improved/ext4recover_v5.c::recover_block_to_file`
   * Before write: `intervals_query`. If fully contained, return
     success without writing (treat as "data already on disk").
   * After successful write: `intervals_add`.
2. `improved/aggressive_scan_v5.c::recover_orphaned_extent_block`
   * Pre-pass over all extents in the leaf. If every one is fully
     covered, do not even create the recovery file.
3. `improved/journal_recovery_v5.c::recover_inode_from_journal`
   * Pre-pass on the depth-0 leaf path with the same logic.

Stats reporter in `print_stats`:
```
Dedup intervals: N distinct extents, M blocks total; X writes skipped (Y blocks)
```

**Evidence on real disk.**

* Unit tests (`tests/test_intervals.c`): 21 cases pass — empty queries,
  exact match, sub/superset, adjacent / bridging merge, no-op re-add,
  large 64-bit physical addresses.
* T0a (regression gate): unchanged 1/1 md5 match.
* T-DEDUP-1 (`logs/tdedup1.log`): 600 MB file, `--normal --journal`,
  output is **one** file `big`, md5 match.
  `Dedup intervals: 3 distinct extents, 153600 blocks total`
  (matches filefrag output exactly).
* T-DEDUP-2 (`logs/tdedup2_v5_summary.log`): aggressive over 500 GB,
  **5 leaf headers detected, 3 dumped, 2 skipped → 40 % dedup**.
* T2 (`logs/t2_dedup.log`): v5 `--journal` on 10 single-extent small
  files: **2/10 → 7/10**. The improvement is a side-effect of dedup
  preventing a "size-larger but wrong-content" later journal copy
  from overwriting an earlier-correct dump.

**Files modified.**

```
improved/ext4_common_v5.h        # +include, +recover_context fields
improved/ext4recover_v5.c        # +dedup pre/post in recover_block_to_file,
                                 # +intervals_init/free in main, +stats line
improved/aggressive_scan_v5.c    # +leaf-level pre-pass
improved/journal_recovery_v5.c   # +leaf-level pre-pass
improved/Makefile_v5             # +recovered_intervals.c in SRCS
improved/recovered_intervals.c   # NEW
improved/recovered_intervals.h   # NEW
```

---

### Phase 1 ✅ — v5 regression vs original (T6 bigalloc partial-success)

**Problem.** On a bigalloc filesystem with cluster_size=64 K, deleting
a 600 MB file produced this confusing state:

* `Total files recovered: 0`
* `Files failed: 2`
* But `/tmp/v5_t6dbg/12_file` exists, is **exactly** 629,145,600 bytes,
  and **md5sum matches the original**.

The recovery actually worked. The accounting was wrong.

**Root cause.** `recover_from_extent_tree` synthesizes residual entries
when the on-disk inode shows `eh_entries == 0` (because the kernel
reset it during deletion):

```c
for (slot = 0; slot < 4; slot++)
    if (ei_slots[slot].ei_leaf > 1) entries = slot + 1;
```

In the bigalloc case the first slot was a valid `ei_leaf` pointing at
the real leaf block that contained the file's full extent map.
Slots 2-4, however, contained leftover post-delete garbage — bytes that
formed huge `ei_leaf_hi:ei_leaf` combinations like
`0x2800_0000_0040 = 43980465145856` (a non-existent 175 PB block),
making `pread` fail and `extent_tree_travel` return 0.

Two further layered bugs in v5 amplified the problem:

* `dump_leaf_extent` returned 1 ("success") on `eh_magic` mismatch
  and on `eh_entries > max` — so the *actual* leaf with the right
  extents had already been processed and its data written, but
  trailing garbage paths still returned 0 to the outer loop.
* The outer loop only counted "writeReturn == 1" leafs as success.

**Fix** (in `improved/ext4recover_v5.c`):

1. Validate synthesized slots: a slot only counts if
   `lo > 1 && leaf_blk > 1 && leaf_blk < total_blocks`.
   Stop scanning slots once a clearly-bogus one is encountered
   (avoids walking trailing garbage).
2. `dump_leaf_extent`: return **0** (real failure) on `eh_magic`
   mismatch and on `eh_entries > max`, instead of the prior
   silent-1.
3. `recover_from_extent_tree`: even if `extent_tree_travel` returns 0,
   if the recovery file already has > 0 bytes on disk, treat as
   partial success (return 0 = success). This handles the
   exact case above where slot 1 already wrote the entire file.

**Evidence.**

* `logs/t6.log` (pre-fix): `RESULT: 0/6` (`bigfile` + 5 small files).
* After fix on a clean run:
  ```
  Total files recovered: 1
  Files failed: 1               # lost+found inode 13 — expected to fail
  expected md5: dcdf6508c3502df8b4224568aaf70d72
  got      md5: dcdf6508c3502df8b4224568aaf70d72
  size: 629145600
  ```
* T0a regression: still md5 match — fix did not break the
  non-bigalloc path.

---

### Phase 1 ✅ — v5 regression vs original (`eh_entries == 0` over-filter)

**Problem.** On a freshly-rm-ed 2 GB multi-level extent file, the
original `ext4recover` recovered with `md5sum` match. The shipped v5
`--normal` produced an empty recovery directory.

**Root cause.** Kernel `ext4_ext_rm_idx` performs
`memmove + le16_add_cpu(eh_entries, -1)` and frees the leaf with
`ext4_free_blocks(METADATA|FORGET)`. This decrements `eh_entries`
all the way to 0 in the on-disk inode while leaving the leaf block's
data intact on disk (FORGET prevents the cleared-buffer write from
being checkpointed).

The original program does not look at `eh_entries`; it walks slots
1..4 of `i_block[]` directly and trusts `ei_leaf > 1`. The shipped
v5 main loop had:

```c
if (ext2fs_le16_to_cpu(eh->eh_entries) == 0) continue;
```

— skipping every deleted multi-level inode.

Worse, even removing that filter wasn't enough: v5's
`recover_from_extent_tree` called
`ext2fs_extent_open2(fs, ino, inode, &handle)` with the **already-zeroed
inode** preloaded. libext2fs then trusts that copy and never goes
through journal replay to fetch the pre-deletion inode.

**Fix** (in `improved/ext4recover_v5.c::recover_from_extent_tree`):

* Remove the `eh_entries == 0` skip in the main loop.
* Use `ext2fs_extent_open(fs, ino, &handle)` (no preloaded inode);
  re-read inode through `ext2fs_read_inode(fs, ino, &handle_inode)` so
  libext2fs can resolve via the journal-aware path.
* If the resolved inode still shows `eh_entries == 0`, scan the 4
  i_block index slots and synthesize an entries count from the highest
  slot whose `ei_leaf > 1`.
* Adjust `eh_depth` to ≥ 1 if it was zeroed (residual idx slots imply
  a tree).

**Evidence.**

* T0a: original recovers with `md5 = f1d32d65...` ✅
* v5 `--normal` after fix: `md5 = f1d32d65...` ✅ exact match
* Verbose run shows v5 walks 17 extents that exactly match what
  `filefrag -v` reported on the original file before deletion.

---

### Phase 0 ✅ — D1: original ext4recover does not build on gcc 13

**Problem.** `make` in `original/src/` failed on Ubuntu 24.04 (gcc 13)
with `-Werror=return-type` and `-Werror=int-conversion`.

**Root cause.** Several `static int` functions in `ext4recover.c`
contained bare `return;` statements without a value — was warned in
older gcc, hard error in 13. The original code's logic was unchanged.

**Fix** (in `original/src/Makefile`):

```diff
-CFLAGS = -Werror -O2 -g
+CFLAGS = -Wno-error=return-type -Wno-error=int-conversion -O2 -g
```

This is intentionally a Makefile-only change; the original source
files are kept verbatim so the "golden behavior" of the upstream
program is preserved.

**Evidence.** `logs/t0a_dedup.log` shows the original program
recovers `12_file` of 2,147,483,648 bytes with md5 match.
