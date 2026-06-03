# tests/

Real-disk test framework for ext4recover.

**WARNING**: every script here will *reformat* the device under test.
Do not run on a system disk. Update `ALLOWED_DEVS` in
`lib_recover_test.sh` to match your spare device(s).

## Files

| File | Purpose |
|------|---------|
| `lib_recover_test.sh` | Shared library: device whitelist, prepare_target, record_file, multi-name verify_recovery_baseline. Sourced by every test. |
| `t0a.sh` | **Regression gate** — 2 GB multi-level extent file on `/dev/vdb1`. Recovers with both original and v5 `--normal`. md5 must match. |
| `t0b.sh`, `t0b_v2.sh`, `t0b_v3.sh` | Investigations into how to force a small file to fragment. Result: modern ext4 mballoc strongly resists fragmentation; this is documented as a kernel-semantic limit, not a bug. |
| `t1.sh` | All-mode regression for v5 (normal / journal / orphan / aggressive) on a 2 GB file. |
| `t2.sh` | v5 `--journal` recovery of 10 single-extent small files (4 KB → 4 MB). Original and v5 `--normal` are expected to fail (kernel `rm_leaf` zeroes the in-inode extent). |
| `t2_compare.sh` | A/B comparison of two v5 binaries on T2 inputs. |
| `t3.sh` | Forced-fragmentation negative test — kept for documentation. |
| `t4.sh` | unwritten extent (`fallocate` + partial write). |
| `t5.sh` | journal wrap boundary — exposed v5's noise-recovery limit (Phase 5 target). |
| `t6.sh` | bigalloc cluster=64 K. |
| `tdedup1.sh` | normal+journal combined dedup verify on 600 MB file. |
| `tdedup2.sh` | aggressive cross-phase dedup verify (timeboxed 30 min). |
| `tpar1.sh` | **Phase 3** parallel-vs-serial correctness regression on a 40 GB partition. Asserts byte-for-byte identical output between `--parallel` and the default (serial) path. |
| `tpar2.sh` | **Phase 3** parallel throughput measurement on a 300 GB partition. Demonstrated that parallelization on a single physical disk is a net-negative — see `docs/design-parallel.md` § "2026-06-03 update". |
| `tfile1.sh` | **Phase 4 (natural)** writes a 1 GB file, deletes it, runs aggressive. Confirms the common single-leaf path is undisturbed by Phase 4 and still recovers an md5-matching file. |
| `tfile2.sh` | **Phase 4 (synthetic)** wipes the partition, writes a 64 MB file F, deletes it, then dd-injects a forged depth=1 root + leaf header on free disk so the depth>0 path is deterministically exercised. Asserts: aggressive emits `aggressive_tree_<root>` AND its md5 matches F. |
| `regression.sh` | T0a + T2 + T4 in sequence — quick sanity bundle. |
| `audit.sh` | Manual audit helper used while debugging the framework itself. |
| `test_intervals.c` | C unit test for `recovered_intervals` data structure. |

## Quick start

```bash
# 1. Compile both the original and the improved binaries.
cd original/src && make && cd -
cd improved && make -f Makefile_v5 && cd -

# 2. Build the unit test for intervals (sanity check the dedup module).
cd improved
gcc -O0 -g -Wall -I. ../tests/test_intervals.c recovered_intervals.c \
    -lpthread -o /tmp/test_intervals
/tmp/test_intervals      # should print "ALL TESTS PASSED"

# 3. Run the regression gate (THIS WILL REFORMAT /dev/vdb1).
sudo bash ../tests/t0a.sh
# Expected:
#   RESULT: 1/1 recovered (md5 match after size-trunc)   -- original
#   RESULT: 1/1 recovered (md5 match after size-trunc)   -- v5 --normal
```

## Verify pattern

Every test follows the same skeleton:

```
1. mkfs.ext4 with the right -b / -O / -J options
2. mount /mnt/target
3. dd if=/dev/urandom of=<file> bs=...
4. record_file <file> <manifest>            # captures (ino, size, md5)
5. (optional: filefrag -v <file>)            # confirms extent shape
6. sync; rm <file>; sync
7. umount /mnt/target
8. run ext4recover (original or v5)
9. verify_recovery_baseline <manifest> <recover_dir>
   - searches for {ino}_file or basename(<file>) under <recover_dir>
   - reads via sudo, head -c <size>, md5sum
   - PASS only if md5 matches the manifest entry
```

The verify step **does not depend on** how the recovery program names
its output — `<ino>_file`, `<filename>`, or `aggressive_<block>` are
all probed.

## Known limits of these tests

* mballoc keeps making small files contiguous even on near-full
  partitions; T3 documents the failed attempts. Conclusion: there's
  no reliable way in this framework to *guarantee* a small file
  becomes multi-level. T0a (large file) is the only deterministic
  multi-level case.
* Aggressive scans on multi-TB partitions take hours. `tdedup2.sh`
  is intentionally timeboxed at 30 min — it kills the running v5 and
  validates the partial output. Phase 3 (parallelization) is the
  proper fix.
* The framework relies on `record_file` being called *before* `rm`
  to capture md5. There is no way to retroactively check md5 of a
  file you didn't pre-record.
