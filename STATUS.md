# STATUS — current snapshot

Date: 2026-06-10
Server under test: `/dev/vdb` (10 T) + `/dev/vdc` (10 T) on a Tencent Cloud VM.
Kernel: Ubuntu 6.8.0. e2fsprogs 1.47.0.

## Phase progress

| Phase | Title | State |
|-------|-------|-------|
| 0 | Test infrastructure, capability detection, 4-era regression matrix | ✅ DONE |
| A | Correctness fixes (A1–A5) | ✅ DONE |
| B | Infrastructure + performance (B1–B4) | ✅ DONE |
| C | New capabilities (C1–C9) | ✅ DONE |
| D | Dead-code removal, minor fixes | ✅ DONE |

## Headline results (v0.6)

| Test | Baseline (audit_v1) | v0.6 |
|------|---------------------|------|
| t0a — 2 GB multi-level (gold gate) | ✅ | ✅ |
| tflexbg — 48 files spread across block groups, `--journal` | 0/48 | **48/48** |
| tjver X/Y — version selection correctness | ✗ | ✅ |
| 4-era matrix E1 (^flex_bg, ^csum) | 6/11 | **11/11** |
| 4-era matrix E2 (^csum) | 10/11 | **11/11** |
| 4-era matrix E3 (current mkfs defaults) | 5/11 | **11/11** |
| 4-era matrix E4 (orphan_file + fast_commit) | 4/11 | **11/11** |

## Source map (improved/)

| File | Role |
|------|------|
| `ext4recover_v5.c` | main, mode dispatch, normal-mode extent walker, `recover_block_to_file` |
| `journal_recovery_v5.c` | jbd2 scanner, per-inode recovery, version selection |
| `aggressive_scan_v5.c` | full-disk magic scan, Phase 4 depth>0 file-level reconstruction |
| `orphan_recovery_v5.c` | classic `s_last_orphan` chain walker |
| `extent_validator_v5.c` | extent header / index sanity checks |
| `utils_v5.c` | bigalloc init, filename mapping + ghost-dirent + hash table, checkpoint |
| `recovered_intervals.c/h` | sorted/merged physical-block interval set (cross-phase dedup) |
| `parallel_scan.c/h` | reader/worker/writer pipeline; opt-in via `--parallel` |
| `fs_capabilities.c/h` | **NEW** on-disk feature-flag detection; gates all era-dependent paths |
| `journal_index.c/h` | **NEW** B2: fs_block→journal copy versioned index |
| `revoke_scan.c` | **NEW** C1: jbd2 revoke-block guided targeted recovery (`--targeted`) |
| `ghost_dirent.c` | **NEW** C7: pre-deletion dirent remnant scanner |
| `orphan_file.c` | **NEW** C4: kernel 5.15+ orphan file recovery |
| `inline_data.c` | **NEW** C6: EXT4_INLINE_DATA_FL recovery from journal |
| `indirect_recovery.c` | **NEW** C8: indirect-block (ext3/old ext4) recovery from journal |
| `content_probe.c` | **NEW** C9: post-dump type sniffing + manifest.tsv |
| `ext4_common_v5.h` | shared types, `recover_context`, capability struct |
| `Makefile_v5` | links `-lext2fs -lcom_err -lpthread -lm` |

## Test framework (tests/)

| File | Purpose |
|------|---------|
| `lib_recover_test.sh` | device whitelist, prepare_target, record_file, verify |
| `t0a.sh` | **Gold gate**: 2 GB multi-level extent, original + v5 --normal |
| `tcompat_matrix.sh` | **NEW** E1–E4 four-era regression matrix |
| `tflexbg.sh` | **NEW** A1 validation: 48 files across flex_bg block groups |
| `tjver.sh` | **NEW** A2 validation: version selection (live vs artifact) |
| `trevoke.sh` | **NEW** C1 validation: revoke-guided targeted recovery |
| `t2.sh` | journal recovery on 10 single-extent small files |
| `t4.sh` | unwritten extent (fallocate partial write) |
| `t6.sh` | bigalloc cluster=64 K |
| `tdedup2.sh` | aggressive cross-phase dedup verify |
| `regression.sh` | T0a + T2 + T4 quick sanity bundle |

## Known limits / non-bugs

1. **Single-extent small file deletion** is unrecoverable by `--normal` / `--aggressive`.
   Kernel `ext4_ext_rm_leaf` zeroes the in-inode extent before journaling. Only
   `--journal` can save them, within the journal's retention window.
2. **mballoc resists fragmentation.** Modern ext4 finds contiguous space even
   on near-full filesystems; forced multi-extent small files are hard to produce.
3. **`--parallel` is opt-in, not a win on a single disk.** Serial scan runs at
   ~92 % of the device's raw sequential-read bandwidth. Worker threads add
   synchronization overhead. Useful on striped / NVMe arrays.
4. **`--targeted` speedup depends on tree depth.** For files with only a few
   single-extent leaves, revoke records are sparse and timing matches `--journal`.
   Multi-TB disks with thousands of freed extent-tree nodes see the largest gain.
