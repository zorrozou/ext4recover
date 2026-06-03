# CHANGELOG

Format: each entry includes the bug, root cause (with kernel/source
evidence), the fix, and the real-disk evidence proving the fix works.

---

## [audit_v1] — 2026-06-03

### Audit ✅ — Five code-walkthrough fixes

A full read-through of all 3792 LoC after Phase 5 surfaced five
defects that were not caught by any existing test. None of them
caused a current real-disk test failure, but each is a latent
correctness or maintainability liability worth fixing now while the
codebase is fresh in our heads.

#### B1 — `recover_block_to_file` chunked IO

**Problem.** Inner loop did `read(devfd, buf, blocksize)` then
`write(inofd, buf, blocksize)` for each 4 KB block in an extent.
A 32768-block (128 MB) extent triggered ~65 K syscalls; a 524288-block
(2 GB) extent triggered ~1 M syscalls.

**Fix.** Replaced with a 1 MB chunked `pread()` + `pwrite()` loop.
Removes the implicit lseek state, makes the function thread-safe by
construction, and reduces syscall count ~256×.

**Speed result.** Did NOT bring the expected speed-up on the cloud
test disk — both old and new versions converge on ~277 s for a
cold-cache 2 GB recovery. Cause: vdb1's raw read bandwidth is
~7.5 MB/s × 2048 MB ≈ 273 s, so the syscall overhead the old code
spent was hidden under IO wait time anyway. The patch still stands
because it produces cleaner code and would benefit a fast disk
(NVMe / striped array) where syscall cost matters.

#### B2 — Phase 5 incomplete: `recover_inode_from_journal` size gate

**Problem.** `journal_recovery_v5.c` had two `do_replace` blocks
(one in the depth=0 path, one in depth>0) that compared the new
tmpfile's size against the existing-on-disk file's size and only
renamed the new file in if it was *bigger*. This was Phase 5's
predecessor logic and survived the Phase 5 patch — meaning Phase 5
could correctly decide "this newer seq beats the recorded version"
in `should_skip_for_seq`, write the data into a tmpname, and then
silently get vetoed by the size gate at rename time.

The exact failure mode: a `truncate(file, 0); write(...); rm` in
the user's workflow produces a newer-seq, smaller-size journal
record. Phase 5 picks it; the size gate discards it; the recovered
file is the older-seq larger version. The user sees stale data with
no indication.

**Fix.** Removed both `do_replace` size-comparison blocks. Trust
`should_skip_for_seq` upstream — it already decided this candidate
should win. `O_TRUNC` re-open + `rename()` is now unconditional for
any candidate that reaches this code.

#### B3 — `eh_depth` byte-order safety

**Problem.** `extent_validator_v5.c:49`:
```c
if (eh->eh_depth > 5)
    return 0;
```
`eh_depth` is a `__le16` field. The comparison is against a CPU-native
constant. On little-endian (x86 / ARM-LE) the raw value happens to
match its little-endian encoding for small numbers, so this works.
On big-endian (POWER/MIPS-BE) `eh_depth = 1` reads as 0x0100 = 256,
permitting any depth. The opposite-direction skew can also reject
valid depths.

**Fix.** Wrap with `ext2fs_le16_to_cpu()`. 1-line.

#### B4 — O(N_groups) linear scan in journal hot path

**Problem.** `calc_inode_from_block()` iterated *every* block group
(up to thousands on a multi-TB FS) on every call, just to find which
group the given fs_block belonged to. Called once per inode in every
journal inode-table block — easily millions of calls on a real run.

**Fix.** Compute the group index in O(1) via
`g = fs_block / s_blocks_per_group`, then verify membership in that
group's inode table range.

#### B5 — Help text out of date

`usage()` still printed `--no-parallel` after Phase 3 flipped the
default. Changed to `--parallel`. 1-line.

#### Validation

| Test | Result |
|------|--------|
| T0a regression gate (2 GB original + v5 normal) | ✅ both md5 match |
| T-FILE-2 (Phase 4 synthetic depth=1) | ✅ aggressive_tree md5 match |
| T-JSEQ-AB (jseq_v1 vs audit_v1, identical snapshot) | ✅ both 5/10, no regression |
| T-AUDIT-SPEED (cold cache, 2 GB --normal) | dedup_v1: 277s, audit_v1: 277s — equal (IO-bound) |

**Frozen baselines.**
* `improved/baselines/ext4recover_v5.c.audit_v1`
* `improved/baselines/journal_recovery_v5.c.audit_v1`
* `improved/baselines/extent_validator_v5.c.audit_v1`

This is the last code change planned for this project.

---

## [jseq_v1] — 2026-06-03

### Phase 5 ✅ — Journal sequence-aware version selection

**Problem.** When a deleted inode appears more than once in the
on-disk journal (typical for files that were overwritten or
truncate+rewritten before deletion), the previous v5 rule picked the
version with the **largest `i_size`** via
`already_recovered_larger()`. This is wrong because:

* "Largest size" can be a transient mid-write state (the file was
  briefly bigger before being truncated to its final smaller size).
* "Largest size" can be a completely unrelated later inode whose
  reuse just happened to be larger (e.g. `noise.bin` written into the
  same inode slot after the user's file was deleted) — T5 hit
  exactly this trap.
* The correct semantic for "latest filesystem state of this inode"
  is "the version recorded by the newest jbd2 transaction". jbd2
  already tags every descriptor block with `h_sequence`, which v5
  was reading into a local `seq` variable and then throwing away.

**Fix.** Plumb the transaction sequence into the selection logic:

```c
/* journal_scan_state now also holds recovered_seqs[]. */
struct journal_scan_state {
    ...
    __u32 *recovered_inodes;
    __u64 *recovered_sizes;   /* kept for stats only */
    __u32 *recovered_seqs;    /* NEW: jbd2 h_sequence */
    ...
};

/* New decision: skip only if we already have an equal-or-newer seq. */
static int should_skip_for_seq(state, ino, seq) {
    for each recorded entry of ino:
        if recovered_seqs[i] >= seq: return 1;  /* skip */
        return 0;                                /* newer wins */
    return 0;
}

/* New record: also write the seq. */
static void mark_recovered_with_seq(state, ino, size, seq);
```

The descriptor's `seq` is propagated through
`process_inode_table_block(state, buf, fs_block, seq)` from the
journal-scanning loop (where `__u32 seq = be32_to_cpu(header->h_sequence);`
was already computed but unused).

Re-dump safety: `recover_inode_from_journal()` already opens output
files with `O_TRUNC`, so accepting a newer-seq candidate naturally
overwrites the previously-dumped older-seq file with no extra
bookkeeping.

**Real-disk validation.**

* `tjseq1.sh` — writes V1=32MB / V2=128MB / V3=64MB versions of the
  same file with sync between each, then deletes. The new DEBUG line
  format `Found deleted inode 12 in journal (seq=2 size=33554432 ...)`
  proves the seq-aware code path actually runs.
* `tjseq_ab.sh` — A/B/C strict comparison on **identical disk
  snapshots** (captured into `tjseqab.img`, restored via loop) across
  `dedup_v1` / `tree_v1` / `jseq_v1`:

  ```
  dedup_v1 : 5/10 md5-matching files recovered
  tree_v1  : 5/10 md5-matching files recovered
  jseq_v1  : 5/10 md5-matching files recovered   <- no regression
  ```

* T0a regression gate green (`logs/t0a_jseq.log`).

**Honest caveat.** In the simple test scenarios above, jbd2 only
retained one deleted-with-extents candidate per inode (older versions
had already been checkpointed out), so both the old size-largest rule
and the new seq-aware rule converge on the same candidate, and they
score identically. The new rule's distinctive behavior only fires
when two or more candidates of the same inode genuinely co-exist in
the journal — which is exactly the case where the old rule was wrong
and silent. This patch eliminates that silent-failure mode at the
cost of zero functionality.

**Frozen baselines.**
* `improved/baselines/ext4recover_v5.c.jseq_v1`
* `improved/baselines/journal_recovery_v5.c.jseq_v1`

---

## [tree_v1] — 2026-06-03

### Phase 4 ✅ — Aggressive depth>0 file-level reconstruction

**Problem.** `aggressive_scan_v5.c` previously bailed out on every
extent-header block whose `eh_depth != 0`:

```c
if (eh->eh_depth != 0) {
    LOG_DEBUG(... "not a leaf extent block, skipping");
    return 0;          /* abandon the block entirely */
}
```

Consequence: when a deleted file's tree was deep enough that ext4
allocated an *independent* depth>0 index node on disk (rare but not
impossible — needs the tree to be depth>=2, i.e. >340 extents on a
4 KB-block FS), aggressive would either drop that index block entirely
or, worse, emit each individual leaf as a standalone
`aggressive_<leaf>` fragment with no way to know they belonged to the
same file. The user had to manually reassemble.

**Fix.** Replace the early-return with a dispatch to a new function:

```c
if (eh->eh_depth != 0) {
    return recover_orphaned_extent_tree(ctx, block_num, buf);
}
```

`recover_orphaned_extent_tree()` calls a recursive `walk_extent_tree()`
(depth-bounded to 5, the ext4 maximum) that:

1. Validates the current header (`validate_extent_header`).
2. If `eh_depth == 0`: appends every valid extent to the collected set,
   with overflow / device-range / uninit checks.
3. If `eh_depth >= 1`: for each `ext3_extent_idx`, checks that
   `ei_leaf` is in `[2, total_blocks)`, reads that block, verifies its
   magic, recurses. Each followed leaf is also marked recovered so the
   linear scan won't re-emit it as a standalone fragment.

After walking, the collected extents are sorted by `ee_block` (logical
order) and dumped sequentially into one file named
`aggressive_tree_<root_block>`. Dedup is consulted in the same way as
the leaf path: if every extent is fully covered already, the whole
tree is skipped silently.

**Real-disk validation (T-FILE-2, synthetic).**

To exercise the depth>0 path deterministically we forge it on a clean
partition:

```
1. mkfs /dev/vdb7 (40 GB ext4); write a 64 MB file F; remember its
   physical extent <34816..51199, 16384 blocks>.
2. Delete F, umount.
3. dd into block 1000010: a depth=0 leaf header with 1 extent ->
   34816..51199.
4. dd into block 1000000: a depth=1 root header with 1 idx -> 1000010.
5. Run ext4recover_v5 --aggressive --verbose.
```

Result:

```
[INFO] Found orphaned extent tree root at 1000000, depth=1 entries=1
[INFO] Reconstructed tree at 1000000 into aggressive_tree_1000000 (1 extents)

expected md5: 5df7f9ae046e80f52381e952dbc3e685
got      md5: 5df7f9ae046e80f52381e952dbc3e685   <- byte-identical
size:         67108864  (64 MB exactly)
```

Both the new INFO lines and the `aggressive_tree_*` filename prove the
Phase 4 code path executes; the md5 match proves the reconstruction
preserved data integrity through index -> leaf -> physical-block
resolution.

**Regressions checked.**

* T0a (2 GB multi-level extent on /dev/vdb1, the regression gate):
  v5 normal still recovers with md5 match (`logs/t0a_tree_v1.log`).
* T-FILE-1 (1 GB file on /dev/vdb7 with natural ext4 allocation):
  recovery still works through `recover_orphaned_extent_block` (the
  leaf path), demonstrating Phase 4 is purely additive — it does not
  disturb the common-case depth=0 behavior.

**Why this matters in practice.** Genuine on-disk depth>0 index blocks
appear only for trees of depth >= 2, i.e. files with > 340 extents on
4 KB-block ext4. That is rare on healthy filesystems but does occur
on:

* Heavily fragmented backup destinations
* `fallocate`-ed sparse files with many holes
* Long-lived databases whose write pattern produces extreme extent
  fragmentation
* Filesystems whose journal happened to retain such an index node from
  a now-deleted very-large file

For such cases, before this fix the user got N orphaned
`aggressive_<leaf>` files with no map back to a single original; now
they get one `aggressive_tree_<root>` ready to use.

**Frozen baselines.**
* `improved/baselines/ext4recover_v5.c.tree_v1`
* `improved/baselines/aggressive_scan_v5.c.tree_v1`

---

## [parallel_optin] — 2026-06-03

### Phase 3 ⚠️ — Aggressive parallelization implemented but disabled by default

**Goal.** Speed up `--aggressive` on multi-TB disks by parallelizing the
magic-scan: a reader thread does large sequential reads, a worker pool
verifies candidate extent-header blocks in parallel, a writer thread
emits files in deterministic chunk-ID order.

**What got built.**

* `improved/parallel_scan.c` (~370 LoC) implementing the reader → workers
  → ordered-writer pipeline as designed in `docs/design-parallel.md`.
* New CLI flags: `--parallel` (opt-in), `--workers N` (default `ncpu-1`).
* Output is byte-for-byte identical to the serial path by construction
  (writer consumes chunks in strict order; tested explicitly with `diff -r`).

**Correctness validation — T-PAR-1 (40 GB):**

```
2 multi-level-extent big files (600 MB + 300 MB) + 2 small ones
written -> deleted -> aggressive scanned in both modes:

byte-for-byte diff (parallel vs serial output dirs): IDENTICAL
md5 of recovered 600 MB file: e2eb343cc1b12f0ae97700450c84c0ff  ✓ matches manifest
md5 of recovered 300 MB file: 59a228a67d8bce65724decec0f95d0b0  ✓ matches manifest
```

**Performance validation — T-PAR-2 (300 GB):**

| Path | Throughput |
|------|-----------|
| `dd if=/dev/vdb6 bs=8M iflag=direct` (physical ceiling) | **267 MB/s** |
| Serial aggressive (Phase 2 binary) | ~245 MB/s = **92 % of ceiling** |
| Parallel aggressive, 7 workers | **77–155 MB/s** — slower than serial |

**Why it didn't pay off.** Serial scan was already operating at 92 % of
the cloud disk's raw sequential read bandwidth. The CPU-side magic check
was never the bottleneck; adding workers on the same single physical
device cannot create bandwidth, and the extra writer-side `pread`
duplication + synchronization overhead pushed total time *up*.

**Decision.** The pipeline code stays in the tree (it is correct and
deterministic) but is now opt-in:

```c
/* ext4recover_v5.c */
g_ctx.use_parallel = 0;   /* DISABLED by default: IO-bound on single disk */
...
} else if (strcmp(argv[i], "--parallel") == 0) {
    g_ctx.use_parallel = 1;
} else if (strcmp(argv[i], "--workers") == 0) {
    if (i + 1 < argc) g_ctx.n_workers = atoi(argv[++i]);
}
```

The implementation is preserved because there are real scenarios where
it will help (striped RAID, NVMe with deep queue depth, network-attached
scratch). See `docs/design-parallel.md` § "2026-06-03 update" for the
full post-mortem.

**Frozen baseline.** `improved/baselines/ext4recover_v5.c.parallel_optin`.

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
