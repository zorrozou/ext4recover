# STATUS — current snapshot

Date: 2026-06-03
Server under test: `/dev/vdb` (10 T) + `/dev/vdc` (10 T) on a Tencent Cloud VM.
Kernel: Ubuntu 6.8 family.

## Phase progress

| Phase | Title | State |
|-------|-------|-------|
| 0 | Test infrastructure & golden baseline (T0a) | ✅ DONE |
| 1 | Surface and fix v5 regressions vs original | ✅ DONE |
| 2 | Cross-phase dedup (interval-tree) | ✅ DONE |
| 3 | Aggressive parallelization | ⚠️ implemented, **disabled by default** — see below |
| 4 | Aggressive depth>0 file-level reconstruction | ✅ DONE |
| 5 | Journal sequence-aware version selection | ⏳ planned (next) |
| 6 | `--target-inode` / `--target-md5` early exit | ⏳ planned |
| 7 | Robustness (counter unification, `O_DIRECT`, inline_data) | ⏳ planned |

## Phase 3 conclusion — parallelization is NOT a win on a single disk

We implemented the pipeline (`parallel_scan.c`, ~370 LoC: reader → worker pool → ordered writer) and validated **byte-for-byte identical output vs serial** on T-PAR-1 (40 GB, 4 files including two multi-level-extent big files: both md5 match).

Then we measured throughput on a 300 GB partition (T-PAR-2) and against the raw device:

| Path | Throughput | Notes |
|------|-----------|-------|
| Raw `dd` direct-IO seq read (physical ceiling) | **267 MB/s** | `dd if=/dev/vdb6 bs=8M iflag=direct` |
| Serial aggressive scan (Phase 2 baseline) | **~245 MB/s** | 92 % of physical ceiling |
| Parallel aggressive scan, 7 workers | **77–155 MB/s** | NEGATIVE — slower than serial |

**Root cause**: serial scan is already at 92 % of the disk's sequential-read bandwidth. Adding worker threads on the SAME single physical device cannot help — the CPU-side magic check is not the bottleneck. The parallel pipeline only adds synchronization overhead (writer's per-hit `pread` re-read in particular) and *slows things down*.

**Decision**: the parallel code stays in the tree (it's correct and may help on a striped multi-disk array or NVMe), but **default is now serial**. Users opt in with `--parallel [--workers N]`. The exit criterion for Phase 3 has been moved from "≥ 2× speedup" to "disable by default, document why."

See `docs/design-parallel.md` § "2026-06-03 update: post-implementation measurement" for the full evidence table and the conditions under which parallelization *would* pay off (multi-spindle / NVMe / network-attached scratch).

## Current frozen baselines (in `improved/baselines/`)

| File | Meaning |
|------|---------|
| `ext4recover_v5.c.before_normal_fix_20260602_110302` | v5 as received — has the `eh_entries==0` regression bug |
| `ext4recover_v5.c.dedup_v1` | normal-fix + T6-bigalloc-fix + interval-tree dedup |
| `ext4recover_v5.c.parallel_optin` | Phase 3 parallelization, disabled by default |
| `ext4recover_v5.c.tree_v1` | **current** — adds Phase 4 file-level reconstruction (`recover_orphaned_extent_tree` + `walk_extent_tree` in `aggressive_scan_v5.c`) |
| `aggressive_scan_v5.c.tree_v1` | aggressive scanner with Phase 4 dispatch to tree-walker on depth>0 headers |

The active source `improved/ext4recover_v5.c` equals `tree_v1` plus
any in-flight changes; check `git log` for delta.

## Real-disk evidence currently in `logs/`

| Log | What it proves |
|-----|----------------|
| `t0a_dedup.log` | T0a regression gate green after dedup integration; original + v5 normal both md5-match on 2 GB file |
| `t0a_with_newverify.log` | T0a green using upgraded verify function |
| `t1.log` | v5 4 modes (normal/journal/orphan/aggressive) on 2 GB file, all md5 match where applicable |
| `t2.log` | Original T2 result — 2/10 (pre-dedup) |
| `t2_dedup.log` | After dedup: 7/10 (250 % improvement) |
| `t2_compare.log` | A/B between before-fix and after-fix v5 binaries on T2 |
| `t4.log` | unwritten extent (`fallocate` partial-write) — all 3 binaries succeed |
| `t6.log` | bigalloc cluster=64 K — journal/all paths green; pre-fix normal failed; post-fix normal md5-match |
| `tdedup1.log` | 600 MB file under `--normal --journal` combined run, 1 product file emitted |
| `tdedup2.log` | aggressive over 500 GB (timed out at 30 min by design) |
| `tdedup2_v5_summary.log` | filtered key lines from aggressive run: 5 leaf headers found, 3 dumped, 2 skipped |
| `regression.log` | combined T0a + T2 + T4 regression run output |

## Source map (improved/)

| File | Lines | Role |
|------|-------|------|
| `ext4recover_v5.c` | ~620 | main, mode dispatch, normal-mode extent walker (`recover_from_extent_tree`, `dump_leaf_extent`, `extent_tree_travel`), `recover_block_to_file` writer with **dedup hooks** |
| `journal_recovery_v5.c` | ~870 | jbd2 scanner, per-inode recovery, **dedup pre-check on depth-0 leaf** |
| `aggressive_scan_v5.c` | ~610 | full-disk magic scan, leaf-level dedup pre-check, **Phase 4: `walk_extent_tree` + `recover_orphaned_extent_tree` for depth>0 file-level reconstruction** |
| `orphan_recovery_v5.c` | ~130 | orphan list walker (rarely triggered after clean unmount) |
| `extent_validator_v5.c` | ~250 | sanity rules for extent header / index / leaf |
| `utils_v5.c` | ~420 | bigalloc init, filename mapping (incl. journal-dir-block scan), checkpoint json |
| `recovered_intervals.c` | ~170 | sorted/merged physical-block interval set |
| `recovered_intervals.h` | ~30 | API |
| `parallel_scan.c` | ~370 | **NEW** reader/worker/writer pipeline; activated with `--parallel`; see Phase 3 notes |
| `parallel_scan.h` | ~25 | **NEW** API |
| `ext4_common_v5.h` | ~205 | shared types, `recover_context`, includes `recovered_intervals.h` |
| `Makefile_v5` | small | links `-lext2fs -lcom_err -lpthread` |

## Test framework (tests/)

| File | Purpose |
|------|---------|
| `lib_recover_test.sh` | Common library: device whitelist, prepare_target, record_file, multi-name verify |
| `t0a.sh` | **Regression gate**: 2 GB multi-level extent on /dev/vdb1; original + v5 normal |
| `t0b.sh`/`t0b_v2.sh`/`t0b_v3.sh` | Investigations into how to produce fragmented small files (mballoc largely defeats this on modern ext4) |
| `t1.sh` | All-mode v5 regression on 2 GB file |
| `t2.sh` | v5 journal recovery on 10 single-extent small files of varying sizes |
| `t2_compare.sh` | A/B test between two binary baselines on T2 inputs |
| `t3.sh` | Forced fragmentation attempt (negative result, kept for documentation) |
| `t4.sh` | unwritten extent (`fallocate` half-written) |
| `t5.sh` | journal wrap boundary — exposed v5 noise-recovery limit (Phase 5 target) |
| `t6.sh` | bigalloc cluster=64 K |
| `tdedup1.sh` | normal+journal combined dedup verify |
| `tdedup2.sh` | aggressive cross-phase dedup verify (timeboxed 30 min) |
| `regression.sh` | T0a + T2 + T4 in sequence |
| `audit.sh` | Manual audit helper (used during framework debugging) |
| `test_intervals.c` | Unit test for the `recovered_intervals` data structure |

## Known limits / non-bugs (do not "fix" without understanding)

1. **Single-extent small file deletion is unrecoverable by `--normal`/aggressive.**
   This is a kernel-source-confirmed semantic of `ext4_ext_rm_leaf`
   (`store_pblock(0); ee_len=0`). Only journal can save them, and
   journal can only save them if jbd2 happened to retain the
   pre-deletion inode copy (limited window).
2. **mballoc strongly resists fragmentation.** Many of our scripted
   attempts to force a small file to spill into ≥ 5 extents fail —
   modern ext4 finds contiguous space even on near-full filesystems.
   This is also why T0b stays at 0/N for `--normal`.
3. **Aggressive on a multi-TB disk takes hours, and adding threads on
   a single disk does NOT help.** Serial scan already runs at ~92 % of the
   device's raw sequential-read bandwidth (267 MB/s on our test VM).
   The implemented `--parallel` pipeline produced byte-identical output
   but ran 20–50 % *slower* than serial on the same device — it is kept
   in the tree as opt-in for future multi-spindle / NVMe scenarios.
   Time-boxed tests intentionally kill the process; this is documented behavior.

## How to run T0a yourself (the regression gate)

```bash
# On a Linux box with a spare ext4-capable block device of ≥ 4 GB
# (THE DEVICE WILL BE REFORMATTED).
edit tests/lib_recover_test.sh   # update ALLOWED_DEVS to your device
DEV=/dev/sdX                      # your spare device, NOT a system disk
sudo bash tests/t0a.sh
# Expected:
#   ----- VERIFY (original) -----
#     OK   ino=12 size=2147483648 bigfile  -> 12_file
#   RESULT: 1/1 recovered (md5 match after size-trunc)
#   ORIG_RESULT=0
#   ----- VERIFY (v5 --normal) -----
#     OK   ino=12 size=2147483648 bigfile  -> 12_file
#   RESULT: 1/1 recovered (md5 match after size-trunc)
#   V5NORMAL_RESULT=0
```
