# ext4recover — Enhanced ext4 File Recovery

A fork of [CloudSysTech/ext4_recover](https://git.woa.com/CloudSysTech/ext4_recover)
with correctness fixes, performance improvements, new recovery capabilities,
and a complete real-disk test framework.

The guiding rule throughout: **provable correctness on real disks.**
Every change is validated by writing a known file → deleting it →
recovering on a real 10 TB block device → byte-level `md5sum` match.

## Project status

| Phase | Title | State |
|-------|-------|-------|
| 0 | Test infrastructure, capability detection, 4-era matrix | ✅ done |
| A | Correctness fixes (A1–A5) | ✅ done |
| B | Infrastructure + performance (B1–B4) | ✅ done |
| C | New capabilities (C1–C9) | ✅ done |
| D | Dead-code removal, minor fixes | ✅ done |
| 1–5 | Original v5 phases (dedup, parallel, tree, jseq) | ✅ done |
| Audit | Code-walkthrough fixes | ✅ done |

## Headline results (v0.6)

| Test | Before | After |
|------|--------|-------|
| t0a — 2 GB multi-level extent (gold gate) | ✅ | ✅ |
| tflexbg — 48 files across flex_bg groups, `--journal` | **0/48** | **48/48** |
| 4-era matrix E1 (CentOS6-era, ^flex_bg ^csum) | 6/11 | **11/11** |
| 4-era matrix E2 (CentOS7-era, ^csum) | 10/11 | **11/11** |
| 4-era matrix E3 (current mkfs defaults) | 5/11 | **11/11** |
| 4-era matrix E4 (orphan_file + fast_commit) | 4/11 | **11/11** |

## Repository layout

```
.
├── README.md
├── PLAN.md                # per-phase reasoning
├── STATUS.md              # one-screen dashboard
├── CHANGELOG.md           # every fix with root cause + evidence
├── docs/
│   ├── kernel-evidence.md # ext4/jbd2 kernel source citations
│   ├── design-dedup.md
│   ├── design-parallel.md
│   └── test-environment.md
├── original/              # upstream ext4recover (untouched)
├── improved/              # v5-fork with all fixes
│   ├── fs_capabilities.{c,h}        # on-disk format detection
│   ├── journal_index.{c,h}          # jbd2 versioned block index
│   ├── revoke_scan.c                # --targeted: revoke-guided recovery
│   ├── ghost_dirent.c               # ghost dirent filename recovery
│   ├── orphan_file.c                # kernel 5.15+ orphan file
│   ├── inline_data.c                # EXT4_INLINE_DATA_FL recovery
│   ├── indirect_recovery.c          # ext3/old-ext4 indirect blocks
│   ├── content_probe.c              # manifest.tsv type sniffing
│   ├── recovered_intervals.{c,h}    # cross-phase dedup interval tree
│   ├── parallel_scan.{c,h}          # opt-in parallel aggressive scan
│   └── baselines/                   # frozen sources at each milestone
└── tests/
    ├── lib_recover_test.sh           # shared library + device whitelist
    ├── t0a.sh                        # gold regression gate
    ├── tcompat_matrix.sh             # 4-era format matrix
    ├── tflexbg.sh                    # A1 flex_bg validation
    ├── tjver.sh                      # A2 version selection
    ├── trevoke.sh                    # C1 targeted scan
    └── ...                           # t1–t6, tdedup, tpar, etc.
```

## Build

```bash
cd improved/
make -f Makefile_v5
# produces ext4recover_v5
```

Dependencies: `libext2fs`, `libcom_err`, `pthread`, `libm`
(Ubuntu: `apt-get install e2fslibs-dev`).

## Run

```bash
# The device WILL be read (never written); recover_dir must NOT be on it.
sudo ./improved/ext4recover_v5 --all --dir /tmp/RECOVER /dev/sdX
```

Modes:
* `--journal` (default) — recover from jbd2 journal copies
* `--normal` — traverse extent tree from cleared inodes
* `--orphan` — walk the orphan list (classic chain + new orphan_file)
* `--aggressive` — full-disk magic scan (slow on large devices)
* `--targeted` — revoke-guided scan (fast, precise; best for recent deletes)
* `--all` — all of the above in sequence
* `--parallel` — opt-in for `--aggressive` (single-disk IO-bound; see docs)
* `--resume` — continue from last checkpoint
* `--verbose` — debug output

After recovery, `RECOVER_DIR/manifest.tsv` lists every recovered file
with detected type and confidence.

## Compatibility

The tool detects on-disk format capabilities from superblock feature flags
and degrades gracefully:

| Feature flag | When absent |
|---|---|
| `flex_bg` | inode-table lookup uses classic per-group layout |
| `metadata_csum` | csum attribution (C3) silently off |
| `INCOMPAT_INLINE_DATA` | inline-data recovery (C6) skipped |
| `COMPAT_ORPHAN_FILE` | falls back to `s_last_orphan` chain |
| `JBD2_INCOMPAT_FAST_COMMIT` | fast-commit area not parsed |
| jbd2 tag format (v1/csum_v2/csum_v3) | auto-detected from journal sb |

ext3 disks and CentOS 5/6-era ext4 are supported; indirect-block files
(C8) are recovered from journal on those disks.

## How to read this repo

1. **Root cause of each fix?** → `CHANGELOG.md` (bug → kernel citation → fix → evidence)
2. **Current state?** → `STATUS.md`
3. **Why certain decisions were made?** → `PLAN.md`
4. **Kernel evidence for the recovery model?** → `docs/kernel-evidence.md`
5. **Run the regression gate yourself:**

```bash
# edit tests/lib_recover_test.sh to whitelist your spare device
sudo bash tests/t0a.sh
```

## License

Original code from CloudSysTech; license follows upstream.
Modifications in this fork are released under the same terms.
