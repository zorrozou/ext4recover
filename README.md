# ext4recover — Enhanced ext4 File Recovery

This repository contains an enhanced fork of the original
[CloudSysTech/ext4_recover](https://git.woa.com/CloudSysTech/ext4_recover)
project plus a complete real-disk test framework, kernel-level analysis,
and a structured improvement roadmap.

The work in this repo focuses on **provable correctness on real disks**:
every change is validated by writing a known file → deleting it →
recovering on a real 10 TB block device → byte-level `md5sum` match.
No loopback short-cuts, no synthetic toy tests.

## What is in here

```
.
├── PLAN.md                # full improvement & test plan (done + TODO)
├── STATUS.md              # current state of every phase
├── CHANGELOG.md           # every concrete fix / change
├── docs/                  # design notes, kernel evidence, env setup
├── original/              # upstream ext4recover (golden baseline)
├── improved/              # v5-fork with our fixes + dedup
│   ├── recovered_intervals.{c,h}   # NEW: cross-phase interval-tree dedup
│   └── baselines/         # frozen source snapshots per milestone
├── tests/                 # real-disk test framework + scripts + unit tests
└── logs/                  # snapshots of key test runs (with md5 evidence)
```

## How to read this repo

1. **Want to understand WHY** these changes matter?
   Read `docs/kernel-evidence.md` — pulls the actual lines from
   `linux/fs/ext4/extents.c` that justify why multi-level deletion
   leaves data recoverable while single-extent deletion does not.

2. **Want to know WHAT was done** vs what is still TODO?
   Read `PLAN.md` (full plan, every checkbox) and `STATUS.md`
   (compact dashboard).

3. **Want to verify on your own disk?**
   Read `tests/README.md`, then run `tests/t0a.sh` against any
   ≥ 4 GB ext4 partition you don't mind reformatting. The framework
   refuses to touch any device not whitelisted in
   `tests/lib_recover_test.sh`.

4. **Want to skim the deltas?**
   `CHANGELOG.md` lists every concrete fix with before/after evidence.

## Quick status

| Item | State |
|------|-------|
| Original `ext4recover` builds on gcc 13 | ✅ fixed (D1) |
| v5 `--normal` regression bug | ✅ fixed |
| v5 bigalloc `--normal` partial-success bug | ✅ fixed |
| Cross-phase interval-tree dedup | ✅ implemented + tested |
| Aggressive parallelization | ⏳ designed, not implemented |
| File-level reconstruction (depth>0) | ⏳ designed, not implemented |
| journal sequence-aware version selection | ⏳ designed, not implemented |
| `--target-inode` / `--target-md5` early-exit | ⏳ planned |

The most recent **real-disk** evidence:

* T0a (golden baseline): 2 GB multi-level extent file, both
  original and v5 recover with `md5sum` exact match.
* T2: v5 `--journal` improved from 2/10 → **7/10** small files
  recovered after dedup integration.
* T-DEDUP-2: aggressive scan over 500 GB hit 5 leaf headers, dumped 3,
  **40 % deduplication observed** end-to-end on real disk.

## License

The original code is from CloudSysTech; license follows upstream.
Modifications in this fork are released under the same terms.
