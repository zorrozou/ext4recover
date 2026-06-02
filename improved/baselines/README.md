# improved/baselines/

Frozen snapshots of `ext4recover_v5.c` at key points so you can
diff / bisect / revert without git history.

| File | Captured at | What state |
|------|-------------|------------|
| `ext4recover_v5.c.before_normal_fix_20260602_110302` | 2026-06-02 11:03 | v5 as received from the test server. **Has the `eh_entries==0` regression bug** — fails to recover deleted multi-level extent files in `--normal` mode. Useful as a comparison point. |
| `ext4recover_v5.c.dedup_v1` | 2026-06-02 15:24 | v5 after Phase 1 (normal regression fix) + Phase 1 (T6 bigalloc partial-success fix) + Phase 2 (interval-tree dedup). All real-disk tests pass; T0a md5-match green. **The current production-quality baseline.** |

The active source `../ext4recover_v5.c` is identical to `dedup_v1`
unless you can see in-flight changes via `git status`.

## Diff helpers

```bash
# What did Phase 1 change vs upstream v5?
diff -u baselines/ext4recover_v5.c.before_normal_fix_20260602_110302 baselines/ext4recover_v5.c.dedup_v1

# What did Phase 2 (dedup integration) add?
# Compare baselines/dedup_v1 against the post-Phase-1, pre-dedup state
# (no separate snapshot was taken between them; see CHANGELOG.md
# Phase 1 entries for the textual deltas).
```
