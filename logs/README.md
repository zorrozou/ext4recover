# logs/

Snapshots of key real-disk test runs.

Each log was captured during a real `bash tests/<test>.sh` invocation
on the test VM (Tencent Cloud, `/dev/vdb` 10 TB partitioned, `/dev/vdc`
10 TB work disk). Manifests are in `manifests/` (same naming).

## File index

| Log | Test | What it proves |
|-----|------|----------------|
| `t0a_dedup.log` | T0a | Original + v5 normal both md5-match on 2 GB file (after dedup integration). Regression gate green. |
| `t0a_with_newverify.log` | T0a | T0a with the upgraded `verify_recovery_baseline` (multi-name lookup). |
| `t1.log` | T1 | All v5 modes (normal/journal/orphan/aggressive) on 2 GB file; md5 evidence for journal & aggressive. |
| `t2.log` | T2 | Pre-dedup v5 journal recovery: 2/10 small files. |
| `t2_compare.log` | T2 A/B | Side-by-side run of `before_normal_fix` vs `dedup_v1` on T2 inputs. Both 2/10. |
| `t2_dedup.log` | T2 | **After** dedup: v5 journal recovers **7/10** small files (250 % improvement; explanation in `CHANGELOG.md`). |
| `t4.log` | T4 | unwritten extent recovery — all 3 binaries succeed (2/2). |
| `t6.log` | T6 | bigalloc cluster=64 K. Pre-fix: normal failed; journal/all md5-match. (Post-fix evidence in `CHANGELOG.md` text; not separately re-run.) |
| `tdedup1.log` | T-DEDUP-1 | 600 MB file under `--normal --journal`: 1 product file emitted (no duplicate `aggressive_*`), md5 match, `Dedup intervals: 3 distinct extents`. |
| `tdedup2.log` | T-DEDUP-2 | Aggressive over 500 GB, timed out at 30 min. Final products: 1 journal `big` (md5-match) + 3 aggressive_* (orphan data from prior tests on the same partition). |
| `tdedup2_v5_summary.log` | T-DEDUP-2 | Filtered key events from the (700 KB) raw aggressive log: 5 leaf headers found, 3 dumped, 2 skipped → 40 % dedup. |
| `regression.log` | regression | T0a + T2 + T4 in one run. |

## Manifest format (`manifests/`)

```
<inode_number>|<size_bytes>|<md5sum>|<original_path>
```

Captured by `record_file` in `tests/lib_recover_test.sh` before deletion.

## Phase 3 logs (parallelization)

| Log | What it shows |
|-----|---------------|
| `tpar1.log` | Correctness regression: parallel vs serial on 40 GB. `diff -r` between two output dirs is empty; md5 of recovered files matches manifest. |
| `tpar2.log` | Throughput measurement on 300 GB. Parallel @ 7 workers ran slower than serial — the serial scan was already at 92 % of the disk's raw read bandwidth (267 MB/s). |
| `t0a_parallel_optin.log` | Post-Phase-3 regression gate on T0a: default (serial) path still recovers 2 GB file with md5 match. |

## Phase 4 logs (file-level tree reconstruction)

| Log | What it shows |
|-----|---------------|
| `tfile1.log` | Natural-allocation case: a 1 GB file with 5 extents on /dev/vdb7. Recovered through the existing `recover_orphaned_extent_block` (leaf path) with md5 match. Proves Phase 4 is non-disruptive to the common case. |
| `tfile2.log` | Synthetic depth=1 test: forge a root+leaf pair pointing at a deleted file's real data extent. Aggressive emits `aggressive_tree_1000000` with md5 byte-identical to the original 64 MB file. Proves the Phase 4 `walk_extent_tree` code path actually executes and reassembles correctly. |
| `t0a_tree_v1.log` | Post-Phase-4 regression gate: T0a still recovers the 2 GB original file with md5 match. |
