# improved/ — v5 fork with our fixes

This directory holds the v5 source tree (CloudSysTech v0.5 base)
with all the fixes documented in `../CHANGELOG.md` applied, plus the
new `recovered_intervals` module for cross-phase deduplication.

## Build

```bash
make -f Makefile_v5
# produces ./ext4recover_v5
```

Linker flags: `-lext2fs -lcom_err -lpthread`. Verified on Ubuntu 24.04
with `libext2fs-dev` 1.47.0 + `comerr-dev`, gcc 13.

## CLI

```
ext4recover_v5 [options] <device>
  --normal        Traditional extent tree recovery (uses journal-aware
                  inode resolution after Phase 1 fix)
  --journal       Recover from jbd2 journal (default)
  --orphan        Recover from orphan list
  --aggressive    Full-disk extent magic scan
  --all           Use all recovery methods
  --resume        Resume from .ext4recover_checkpoint.json
  --dir <path>    Recovery directory (default: ./RECOVER)
  --verbose       Emit [DEBUG] lines (incl. dedup-skip events)
  --trim          Trim trailing zeros from recovered files
  --version       Show version
```

## File index

Active sources used by `Makefile_v5`:

| File | Role |
|------|------|
| `ext4recover_v5.c` | main, mode dispatch, `recover_from_extent_tree`, `dump_leaf_extent`, `extent_tree_travel`, `recover_block_to_file` (with dedup hooks) |
| `journal_recovery_v5.c` | jbd2 scanner, `recover_inode_from_journal` (with dedup pre-check on depth=0) |
| `aggressive_scan_v5.c` | full-disk magic scanner, `recover_orphaned_extent_block` (with leaf-level dedup pre-check) |
| `orphan_recovery_v5.c` | orphan list walker |
| `extent_validator_v5.c` | sanity rules for extent header / index / leaf |
| `utils_v5.c` | bigalloc init, filename map, journal-dir-block scan, checkpoint json |
| `recovered_intervals.c` / `.h` | **NEW** sorted/merged physical-block interval set |
| `ext4_common_v5.h` | shared types, recover_context (now includes dedup state) |
| `Makefile_v5` | build rules; lists the SRCS above |

Inactive sources (kept for historical / reference only — NOT in the
active build):

| File | Why it's here |
|------|---------------|
| `ext4recover.c` | upstream `original/src/ext4recover.c` mirror, unchanged |
| `ext4recover_v2.c` | early v2 fork |
| `ext4recover_v5_debug.c` | a v5 variant with extra debug output |
| `aggressive_scan.c` | v0 of the aggressive scan |
| `bigalloc_support.c` | superseded by `utils_v5.c::init_cluster_info` |
| `checkpoint.c` | superseded by `utils_v5.c::save_checkpoint/load_checkpoint` |
| `extent_validator.c` | superseded by `extent_validator_v5.c` |
| `ext4_common.h` | superseded by `ext4_common_v5.h` |
| `filename_recovery.c` | superseded by `utils_v5.c` filename mapping |
| `journal_recovery.c` | superseded by `journal_recovery_v5.c` |
| `orphan_recovery.c` | superseded by `orphan_recovery_v5.c` |

If you want a clean tree, delete everything except the files listed
in the "active" table — `Makefile_v5` only references those.

## Frozen baselines

`baselines/` contains source snapshots of v5 at meaningful milestones.
See `baselines/README.md`.
