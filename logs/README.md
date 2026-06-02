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
