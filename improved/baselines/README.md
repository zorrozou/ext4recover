# improved/baselines/

Frozen snapshots of `ext4recover_v5.c` at key points so you can
diff / bisect / revert without git history.

| File | Captured at | What state |
|------|-------------|------------|
| `ext4recover_v5.c.before_normal_fix_20260602_110302` | 2026-06-02 11:03 | v5 as received from the test server. **Has the `eh_entries==0` regression bug** — fails to recover deleted multi-level extent files in `--normal` mode. Useful as a comparison point. |
| `ext4recover_v5.c.dedup_v1` | 2026-06-02 15:24 | v5 after Phase 1 (normal regression fix) + Phase 1 (T6 bigalloc partial-success fix) + Phase 2 (interval-tree dedup). All real-disk tests pass; T0a md5-match green. |
| `ext4recover_v5.c.parallel_optin` | 2026-06-03 08:07 | Adds Phase 3 parallelization (`improved/parallel_scan.c`) but disabled by default — IO-bound on single disk. Adds `--parallel` / `--workers` CLI flags. |
| `ext4recover_v5.c.tree_v1` | 2026-06-03 09:21 | **Current.** Adds Phase 4 file-level reconstruction. Synthetic real-disk validation passes (`logs/tfile2.log`). |
| `aggressive_scan_v5.c.tree_v1` | 2026-06-03 09:21 | The aggressive scanner with `walk_extent_tree` + `recover_orphaned_extent_tree` for depth>0 dispatch. |

The active source `../ext4recover_v5.c` is identical to `tree_v1`
unless you can see in-flight changes via `git status`.

## Diff helpers

```bash
# What did Phase 1 change vs upstream v5?
diff -u baselines/ext4recover_v5.c.before_normal_fix_20260602_110302 baselines/ext4recover_v5.c.dedup_v1

# What did Phase 3 add on top of Phase 2?
diff -u baselines/ext4recover_v5.c.dedup_v1 baselines/ext4recover_v5.c.parallel_optin

# What did Phase 4 add on top of Phase 3?
diff -u baselines/aggressive_scan_v5.c.tree_v1 ../aggressive_scan_v5.c
```
