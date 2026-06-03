# ext4recover — Enhanced ext4 File Recovery

A fork of [CloudSysTech/ext4_recover](https://git.woa.com/CloudSysTech/ext4_recover)
with five rounds of correctness fixes and improvements, plus a
complete real-disk test framework and post-mortem documentation for
every decision (including the negative ones).

The guiding rule throughout: **provable correctness on real disks.**
Every change is validated by writing a known file → deleting it →
recovering on a real 10 TB block device → byte-level `md5sum`
match. No loopback shortcuts, no synthetic toy tests (except where
synthetic injection was the only way to deterministically exercise a
specific code path, e.g. T-FILE-2's forged depth=1 root block).

## Project status: complete

This project is no longer being actively developed. The five planned
phases that survived case-by-case evaluation are all done and
validated; two additional phases were explicitly evaluated and
declined; a final code-walkthrough audit fixed five latent defects.

| Phase | Title | State |
|-------|-------|-------|
| 0 | Test infrastructure & golden-baseline T0a | ✅ done |
| 1 | Surface and fix v5 regressions vs. original | ✅ done |
| 2 | Cross-phase interval-tree deduplication | ✅ done |
| 3 | Aggressive scan parallelization | ⚠️ implemented, **disabled by default** (single-disk IO bound; see below) |
| 4 | Aggressive depth>0 file-level reconstruction | ✅ done |
| 5 | Journal sequence-aware version selection | ✅ done |
| 6 | Targeted recovery & early exit | ❌ decided not to do (low value) |
| 7 | Smaller robustness items | ❌ decided not to do (item-by-item) |
| Audit | Code-walkthrough fixes (5 items) | ✅ done |

For the full reasoning behind each decision, see `PLAN.md`.

## Repository layout

```
.
├── README.md              # this file
├── PLAN.md                # roadmap with every Phase's status + reasoning
├── STATUS.md              # snapshot of current state, baselines, test matrix
├── CHANGELOG.md           # every concrete fix - bug, root cause, validation
├── docs/                  # design docs and kernel-source evidence
│   ├── kernel-evidence.md
│   ├── design-dedup.md
│   ├── design-parallel.md  # incl. 2026-06-03 negative-result post-mortem
│   └── test-environment.md
├── original/              # upstream ext4recover (golden baseline, untouched)
├── improved/              # the v5-fork with all our fixes
│   ├── recovered_intervals.{c,h}    # NEW Phase 2 module
│   ├── parallel_scan.{c,h}          # NEW Phase 3 module (opt-in)
│   ├── aggressive_scan_v5.c         # Phase 4 tree walker added here
│   ├── journal_recovery_v5.c        # Phase 5 seq-aware selection here
│   └── baselines/                   # frozen sources at each milestone
├── tests/                 # real-disk + synthetic test scripts
└── logs/                  # captured key test outputs (with md5 evidence)
```

## How to read this repo

1. **Want to understand WHY each change matters?**
   Read `docs/kernel-evidence.md` first - it pulls actual lines from
   `linux/fs/ext4/extents.c` that justify why multi-level deletion
   leaves data recoverable while single-extent deletion does not.

2. **Want the project's full story (what was done, what was deferred,
   what was rejected, and why)?**
   Read `PLAN.md` - every Phase has its motivation, work checklist,
   real-disk validation, and final verdict.

3. **Want a compact dashboard?** `STATUS.md` has it on one screen.

4. **Want every concrete fix with before/after evidence?**
   `CHANGELOG.md` is per-fix: bug → root cause (with kernel/source
   citation when relevant) → fix → real-disk validation.

5. **Want to verify on your own disk?**
   Read `tests/README.md`, then run `tests/t0a.sh` against any
   ≥ 4 GB ext4 partition you don't mind reformatting. The framework
   refuses to touch any device not whitelisted in
   `tests/lib_recover_test.sh`.

## Headline results

* **T0a (golden gate, 2 GB multi-level extent file)** — both the
  original `ext4recover` and our v5 fork recover with byte-level
  `md5sum` match. This is the regression gate that runs after
  every change.

* **T2 (single-extent small files via `--journal`)** — improved from
  2/10 to **7/10** after Phase 2 dedup integration.
  See `logs/t2_dedup.log`.

* **T-DEDUP-2 (aggressive on 500 GB)** — 5 leaf headers found, 3
  dumped, 2 skipped by interval-tree dedup; **40 % redundant work
  eliminated** end-to-end on a real disk. See
  `logs/tdedup2_v5_summary.log`.

* **T6 (bigalloc cluster=64K, 600 MB file via `--normal`)** — was
  silently broken (recovered correct data but reported as failed).
  Now completes with `md5sum` match.

* **T-FILE-2 (Phase 4, synthetic depth=1)** — aggressive scan
  follows the forged index→leaf chain and reconstructs a single
  `aggressive_tree_<root>` file with `md5sum` byte-identical to
  the deleted original. See `logs/tfile2.log`.

* **Phase 3 negative result, documented**: the parallel pipeline
  produces byte-identical output to the serial path (verified by
  `diff -r`) but runs **slower** on a single physical disk
  (~155 MB/s parallel vs. ~245 MB/s serial; raw device ceiling is
  267 MB/s). The serial path was already at 92 % of the raw read
  bandwidth, so worker threads cannot help. Default is now serial;
  `--parallel` is kept as opt-in for striped/NVMe scenarios.
  See `docs/design-parallel.md` § "2026-06-03 update".

* **Audit (post-Phase-5 code walkthrough)** — five concrete
  defects fixed: chunked IO, removal of stale Phase-5-defeating
  size gate, byte-order safety, O(N)→O(1) hot-path lookup, stale
  help text. See `CHANGELOG.md` `[audit_v1]` entry for full
  before/after.

## Build

```bash
cd improved/
make -f Makefile_v5
# produces ext4recover_v5
```

Dependencies: `libext2fs`, `libcom_err`, `pthread` (Ubuntu:
`apt-get install e2fslibs-dev`).

## Run (always against a spare/expendable device)

```bash
# the device WILL be reformatted; tests/lib_recover_test.sh has a
# whitelist that prevents accidents
sudo ./improved/ext4recover_v5 --all --dir /tmp/RECOVER /dev/sdX
```

Modes:
* `--journal` (default) - recover from jbd2 journal copies
* `--normal` - traverse extent tree from cleared inodes (residual
  index entries)
* `--orphan` - walk the orphan list
* `--aggressive` - full-disk magic scan (slow on large devices)
* `--all` - all of the above in sequence
* `--parallel` - opt-in for `--aggressive` (see Phase 3 caveats)
* `--resume` - continue from last checkpoint
* `--verbose` - debug output

## License

The original code is from CloudSysTech; license follows upstream.
Modifications in this fork are released under the same terms.
