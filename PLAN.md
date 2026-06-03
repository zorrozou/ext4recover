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

## 4. Phase 3 — Aggressive parallelization (implemented, **disabled by default**)

Goal (original): 4 T scan from ~4.5 h → ~2 h while preserving exact byte-level
output (regression-tested against Phase 2 baseline).

Design (see `docs/design-parallel.md`):

```
[reader thread]                  [N worker threads]              [writer thread]
seq pread64 64 MB chunks      magic check + sanity validate      query intervals,
into ringbuffer (mmap)        per 4 K block; on hit enqueue      dump if not covered,
                              hit candidate                      register interval
```

Implementation:

* [x] Built the pipeline in `improved/parallel_scan.c` (~370 LoC).
* [x] Reader → bounded chunk queue → workers (each worker takes a whole chunk,
      scans every 4 KB block inside it serially) → writer consumes hit lists
      in strict chunk-id order, guaranteeing the output is identical to the
      serial path.
* [x] New CLI: `--parallel` (opt-in), `--workers N` (default `ncpu-1`).
* [x] **Correctness regression PASSED** on T-PAR-1 (40 GB, two multi-level
      extent files): `diff -r` between parallel and serial output dirs is
      empty; md5 of recovered files matches manifest.

Performance result (T-PAR-2, 300 GB partition):

| Path | Throughput |
|------|-----------|
| Raw `dd` direct-IO seq read (physical ceiling) | **267 MB/s** |
| Serial aggressive scan (Phase 2 binary) | **~245 MB/s** = 92 % of ceiling |
| Parallel aggressive scan, 7 workers | **77–155 MB/s** — slower than serial |

**Decision**: the serial path was already at 92 % of the disk's raw read
bandwidth, so the CPU-side magic check was never the bottleneck. Adding
worker threads on the same single physical disk produces a *negative*
result (extra pread + synchronization overhead). The parallel code is
preserved for environments where it *would* help (striped multi-spindle
arrays, NVMe with deep queue depth, network-attached storage), but
**the default is now serial**.

Status: ✅ implemented, ⚠️ disabled by default. Frozen baseline:
`improved/baselines/ext4recover_v5.c.parallel_optin`.

See `docs/design-parallel.md` § "2026-06-03 update" for the full
post-mortem and the conditions under which `--parallel` would pay off.

## 5. Phase 4 — File-level reconstruction (depth>0 traversal in aggressive)

Previously `recover_orphaned_extent_block` early-exited when
`eh_depth != 0`, so a file whose tree included an independent
depth>0 index block on disk was either ignored or its leaves were
each emitted as a separate `aggressive_<block>` fragment. The user
had no way to know they belonged together.

* [x] When aggressive scan finds a depth>0 header, dispatch to a new
      `recover_orphaned_extent_tree()` instead of returning 0.
* [x] Implemented `walk_extent_tree()` (recursive, capped at depth 5)
      that follows each `ei_leaf_hi:ei_leaf` to its leaf, validates
      each level's header (magic + sane entries + ei_leaf in range
      `[2, total_blocks)`), and collects every leaf extent.
* [x] Collected extents are sorted by logical block, then dumped as
      ONE consolidated file `aggressive_tree_<root_block>` instead of
      N independent fragments.
* [x] Leaf blocks reached via the tree walk are marked recovered so
      the linear scan won't later re-emit them as standalone
      `aggressive_<leaf>` files.
* [x] Dedup pre-check: if every collected extent is already covered by
      `recovered_extents`, the whole tree is skipped without
      creating an empty file.
* [x] Synthetic real-disk validation (T-FILE-2): forge a depth=1 root
      block in a free area of `/dev/vdb7` pointing to a forged leaf
      that in turn points to a deleted file's real data extent. Run
      aggressive scan, verify `aggressive_tree_<root>` is emitted
      with md5 byte-identical to the original file.

Real-disk evidence (`logs/tfile2.log`):

```
[INFO] Found orphaned extent tree root at 1000000, depth=1 entries=1
[INFO] Reconstructed tree at 1000000 into aggressive_tree_1000000 (1 extents)
expected md5: 5df7f9ae046e80f52381e952dbc3e685
got      md5: 5df7f9ae046e80f52381e952dbc3e685  ✓
size: 67108864 (64MB)                            ✓
```

Note: in practice a depth>0 *index* node on disk (outside the inode)
only exists when the file's tree is depth>=2 (i.e. >340 extents on a
4KB-block FS) - which is rare for normal workloads. The far more
common depth=0/depth=1 case is still handled by the original leaf path
and continues to work (T-FILE-1 1 GB file still recovered with md5
match through `recover_orphaned_extent_block`). Phase 4 is the
mechanism that *would* save the user from a fragmented manual reassembly
when an independent index block does survive on disk.

Status: ✅ implemented + validated. Frozen baseline:
`improved/baselines/ext4recover_v5.c.tree_v1` /
`improved/baselines/aggressive_scan_v5.c.tree_v1`.

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
