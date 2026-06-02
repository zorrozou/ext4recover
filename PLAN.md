# ext4recover Improvement & Test Plan

## 0. Background

Two source bases exist in this repo:

* **`original/`** — upstream `ext4recover.c` (~450 lines).
  Strategy: walk every inode in the inode table; for deleted regular
  files (`links_count==0`) whose root header in `i_block[]` still has
  residual extent_idx slots (`ei_leaf > 1`), traverse the tree and dump
  each leaf's extents directly from the block device. Relies on the
  ext4 `ext4_ext_rm_idx` → `ext4_free_blocks(METADATA|FORGET)` path
  leaving leaf data physically intact even after deletion.

* **`improved/`** — v5 fork (~5,500 lines across multiple files).
  Adds: orphan list path, jbd2 journal scan (recovers single-extent
  files the original cannot), aggressive full-disk extent-magic scan,
  filename mapping from directory blocks, bigalloc, checkpoint/resume.
  But: was previously only validated on **loopback images ≤ 10 MB**.

This plan drives both bases through real-disk validation on
two 10 TB block devices and applies fixes in priority order.

## 1. Phase 0 — Test infrastructure & golden baselines

Goal: prove the **original** program actually recovers on real hardware
and establish an unbreakable regression gate.

* [x] Carve `/dev/vdb` into 6 GPT partitions (4 T / 2 T / 500 G ×3 / 300 G)
      for different block-size / bigalloc / small-journal scenarios.
* [x] Mount `/dev/vdc` as the work disk for recovery output, manifests, logs.
* [x] Implement test framework `tests/lib_recover_test.sh` with hard
      device whitelist (`vdb*`/`vdc` only) so accidental commands cannot
      damage the OS disk.
* [x] **T0a**: 2 GB multi-level extent file → rm → original recovers
      → `md5sum` byte-level match.
      **This is the regression gate; every later change must keep T0a green.**
* [x] **T0b**: same flow on small files; established that small files almost
      always become single-extent on modern ext4 (mballoc tries hard to
      give contiguous space) → original/v5-normal **must** fail per
      kernel `ext4_ext_rm_leaf` semantics; only journal can save them.

Status: ✅ complete. Evidence: `logs/t0a_dedup.log`, `logs/t0a_with_newverify.log`.

## 2. Phase 1 — Surface and fix v5 regressions versus original

These are bugs where v5 lost capability the original program had.

* [x] **D1**: original fails to compile on gcc 13 (`-Werror` +
      `return;` in non-void function). Fixed in `original/src/Makefile`
      by relaxing to `-Wno-error=return-type -Wno-error=int-conversion`.
* [x] **v5 normal regression #1**: `eh_entries == 0` early-skip in
      main loop excluded every deleted multi-level inode (because
      `ext4_ext_rm_idx` decrements entries to 0 in the on-disk inode
      while leaf data on disk is preserved by FORGET).
      Fix: change `recover_from_extent_tree` to use
      `ext2fs_extent_open(fs, ino, &handle)` (NO preloaded inode) so
      libext2fs resolves through the journal-aware path, then re-read
      inode and synthesize residual entries from i_block[] slots whose
      `ei_leaf > 1`. T0a md5 match restored.
* [x] **v5 normal regression #2** (T6 bigalloc): 600 MB file actually
      recovered with byte-level md5 match, but the function returned
      −1 because synthesized entries scanned trailing garbage idx slots
      whose `ei_leaf_hi/lo` formed huge bogus block numbers, causing
      `pread` failure on slot 2/3/4 and a "false failure".
      Fix: validate `ei_leaf` falls within
      `[2, total_blocks)` before counting a slot; treat partial-success
      (file already has bytes on disk) as success even when later
      slots fail. Also fixed `dump_leaf_extent` returning 1 (false
      success) on `eh_magic` mismatch — now returns 0.

Status: ✅ complete. Evidence: `logs/t0a_dedup.log`, `logs/t6.log`,
`improved/baselines/ext4recover_v5.c.before_normal_fix_*` and
`improved/baselines/ext4recover_v5.c.dedup_v1`.

## 3. Phase 2 — Cross-phase deduplication

Goal: stop emitting redundant `aggressive_<block>` files and stop
re-writing the same physical extent that an earlier phase already wrote.

* [x] Design: sorted/merged interval set on physical block ranges,
      shared across normal / journal / aggressive phases.
      See `docs/design-dedup.md`.
* [x] Implement `recovered_intervals.{c,h}`: O(log N) binary-search
      query, insert with adjacent-merge, RW lock predeclared (used
      when scan goes parallel).
* [x] Hook into three writers:
      * `recover_block_to_file`: query before write (skip if fully
        covered); register interval after successful write.
      * aggressive `recover_orphaned_extent_block`: pre-pass over the
        leaf's extents — if every one is fully covered, skip leaf
        entirely (no zero-byte file emitted).
      * journal `recover_inode_from_journal` depth-0 path: same pre-pass.
* [x] Stats line in `print_stats`:
      `Dedup intervals: N distinct extents, M blocks total; X writes skipped (Y blocks)`.
* [x] Unit tests `tests/test_intervals.c` — 21 cases (empty / contained /
      adjacent merge / bridging merge / superset / no-op re-add /
      large physical addresses), all PASS.
* [x] Real-disk evidence:
      * **T0a**: still md5-match (regression gate green).
      * **T-DEDUP-1**: 600 MB file via `--normal --journal`,
        `Dedup intervals: 3 distinct extents, 153600 blocks total`,
        single `big` file emitted, md5 match.
      * **T-DEDUP-2**: aggressive over 500 GB,
        **5 leaf headers found, only 3 dumped, 2 skipped → 40 % dedup**.
      * **T2**: v5 `--journal` improved from **2/10 → 7/10** small
        files (dedup prevented "size-larger but wrong" version from
        overwriting correct early dump).

Status: ✅ complete. Frozen baseline: `improved/baselines/ext4recover_v5.c.dedup_v1`.

## 4. Phase 3 — Aggressive parallelization (designed, NOT implemented)

Goal: 4 T scan from ~4.5 h → ~2 h while preserving exact byte-level
output (regression-tested against Phase 2 baseline).

Design (see `docs/design-parallel.md`):

```
[reader thread]                  [N worker threads]              [writer thread]
seq pread64 64 MB chunks      magic check + sanity validate      query intervals,
into ringbuffer (mmap)        per 4 K block; on hit enqueue      dump if not covered,
                              hit candidate                      register interval
```

* [ ] Replace per-block `io_channel_read_blk64` (current bottleneck:
      245 MB/s on 4 K syscalls) with a single reader thread doing
      64 MB `pread64` into a double-buffered mmap-backed ring; expected
      raw bandwidth jump to 500–700 MB/s on the same disks.
* [ ] N CPU-only worker threads pull 4 K block views from the ring,
      do `eh_magic == 0xF30A` + `validate_extent_header`. On hit,
      push the candidate (block_num + buffer copy) to a hit queue.
* [ ] Single writer thread is the only one calling `recover_block_to_file`
      and `intervals_add` — no locks needed on hot paths beyond the
      pthread_rwlock already in `recovered_intervals`.
* [ ] Per-segment checkpoint: after each 1 GB segment finishes write-out,
      `fsync` and update checkpoint json (re-uses v5's existing
      `--resume` mechanism, just at segment granularity instead of inode).
* [ ] **Regression**: outputs (file count, names, contents) must be
      identical to single-threaded Phase 2 build on T-DEDUP-2 data.
* [ ] Stretch: per-segment thread-local interval shard, periodic merge
      into global tree, eliminates RW-lock contention if it shows up.

Status: ⏳ design complete, code not written.

## 5. Phase 4 — File-level reconstruction (depth>0 traversal in aggressive)

Currently `recover_orphaned_extent_block` (line 98) early-exits when
`eh_depth != 0`, so a file whose root index points to N leaves becomes
N independent `aggressive_<block>` fragments. The user has to manually
guess which fragments belong together.

* [ ] When aggressive scan finds a depth>0 header, follow each
      `ei_leaf_hi:ei_leaf` to its leaf; gather all leaf extents into
      one consolidated extent set; emit one file instead of N.
* [ ] Compute per-discovered-tree fingerprint =
      `sha1(sort(<ee_block, ee_start, ee_len> per extent))`.
      Maintain a fingerprint set; if a new tree's fingerprint is a
      subset of an existing one, drop it; if a superset, replace.
      This catches the "journal old leaf vs current leaf"
      version-overlap problem your review flagged.
* [ ] Real-disk validation: T1-style 2 GB file should now produce a
      single recovered file from aggressive instead of one per leaf.

Status: ⏳ planned.

## 6. Phase 5 — Journal sequence-aware version selection

T5 surfaced that v5 currently keeps the largest-`i_size` version of
each inode it finds in the journal (`already_recovered_larger`),
which can pick a transient mid-write version or even an unrelated
later inode (e.g. `noise.bin`) if the size happens to be larger than
the user's target. The right primitive is the journal descriptor
sequence number `h_sequence`, which v5 already parses but does not use.

* [ ] In `parse_journal_block`, propagate the surrounding descriptor's
      `h_sequence` to every inode discovered in subsequent inode-table
      blocks of the same transaction.
* [ ] Replace `mark_recovered(state, ino, size)` with
      `mark_recovered(state, ino, size, seq)`: keep the entry with the
      **highest sequence** for which `is_deleted` was observed
      (this is the "deleted" event itself, the most recent state).
* [ ] Optional: prefer entries whose inode appears in the filename
      map (real user file) over orphan inodes (transient noise).
* [ ] Real-disk validation: re-run T5 with this change; expectation —
      the recovered set is the user-target sf files, not noise.bin.

Status: ⏳ planned.

## 7. Phase 6 — Targeted recovery & early exit

* [ ] CLI flags: `--target-inode <ino>` and `--target-md5 <hex>`.
* [ ] When set, aggressive/journal stop as soon as the target's data
      is fully covered by `recovered_intervals` and matches md5.
      T1 evidence: aggressive on 4 T disk hit inode 12's leaves at
      12.8 % → would save 87 % of scan time if we early-exit.

Status: ⏳ planned.

## 8. Phase 7 — Smaller robustness items (low priority)

* [ ] Unify `Files failed` counter to count distinct inodes (currently
      counted per-phase, can double-count system inodes).
* [ ] Optional `O_DIRECT` mode behind a flag for the rare on-line
      recovery case where pagecache may have zeroed leaves.
* [ ] inline_data file recovery (very small files stored in the inode
      itself; v5 currently skips because they have no extent flag).
* [ ] Better stat output: per-extent-source breakdown (normal /
      journal / orphan / aggressive / dedup-skip).

Status: ⏳ planned.

## 9. Test environment reference

See `docs/test-environment.md` for the exact partition layout
(`/dev/vdb1..6`), mkfs options, and safety whitelist.

## 10. Regression matrix

The complete pass/fail table for current code is maintained in
`STATUS.md`. The summary:

| # | Test | Original | v5 normal | v5 journal | v5 aggressive |
|---|------|----------|-----------|------------|---------------|
| T0a | 2 GB multi-level | ✅ md5 | ✅ md5 | — | ✅ md5 (slow) |
| T0b | small file kernel-semantic check | ✅ 0/10 expected | ✅ 0/10 | n/a | n/a |
| T1 | v5 all-modes regression | n/a | ✅ md5 | ✅ md5 | ✅ md5 |
| T2 | single-extent journal recovery | ✅ 0/10 | ✅ 0/10 | ⚠️ 7/10 (real cap) | n/a |
| T4 | unwritten extent | ✅ 2/2 | ✅ 2/2 | ✅ 2/2 | n/a |
| T6 | bigalloc cluster=64 K | n/a | ✅ md5 (after fix) | ✅ md5 | ✅ md5 |
| T-DEDUP-1 | normal+journal dedup | n/a | ✅ md5, 1 file | ✅ md5, 1 file | n/a |
| T-DEDUP-2 | aggressive dedup, 500 GB | n/a | n/a | ✅ md5 | 5→3 (40 % skip) |
