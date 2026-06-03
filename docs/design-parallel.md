# Design — Aggressive Scan Parallelization

Status: ⏳ design complete, **not yet implemented**.

## Why we need this

Single-threaded aggressive scan throughput on the test rig (Tencent
Cloud VM, virtualized SSD/HDD-class block device) is **245 MB/s**.
That is dominated not by the disk but by per-block syscall overhead:
each 4 KB block goes through `io_channel_read_blk64` →
`pread64(devfd, buf, 4096, off)`. 4 T at this rate = ~4.5 hours.

Three independent improvements stack here:

1. **IO batching.** A single 64 MB `pread64` is ~3× cheaper per byte
   than 16384 separate 4 KB `pread64`s (fewer syscalls, more
   readahead-friendly).
2. **CPU/IO overlap.** Magic-check + `validate_extent_header` is
   pure CPU work that can run while the next IO chunk is in flight.
3. **CPU parallelism.** Per-block validation parallelizes
   embarrassingly across N cores once IO is fed in big chunks.

Rough expected speedups (single test rig):

| Configuration | Throughput | Time for 4 T |
|---------------|------------|--------------|
| Current (per-block syscalls) | 245 MB/s | ~4.5 h |
| Big-IO single-thread | ~600 MB/s | ~2 h |
| Big-IO + 4-worker parallel | ~700 MB/s (disk cap) | ~1.6 h |

The disk caps out around 700 MB/s on this VM, so going beyond ~4
worker threads is not productive.

## Architecture: reader → workers → writer pipeline

```
                  ┌───────────────────────────────────┐
                  │ ringbuffer of 64 MB chunks        │
                  │ (mmap-backed, double-buffered)    │
                  └───────────────────────────────────┘
                            ↑                  ↓
        ┌─────────────────┐ │                  │ ┌────────────────────┐
        │ reader thread   │ │                  │ │ N worker threads   │
        │  pread64 64 MB  │─┘                  └─│  per 4 K block:    │
        │  sequentially   │                      │   eh_magic check   │
        │  through device │                      │   validate_extent  │
        └─────────────────┘                      │   on hit → enqueue │
                                                 │     hit_queue      │
                                                 └────────────────────┘
                                                            ↓
                                          ┌──────────────────────────┐
                                          │ writer thread            │
                                          │   intervals_query        │
                                          │   (skip if covered)      │
                                          │   recover_block_to_file  │
                                          │   intervals_add          │
                                          └──────────────────────────┘
```

Why this exact topology:

* **Reader is single-threaded** because head-of-disk seeks ruin
  throughput on rotational/virtualized media. One stream, large
  contiguous reads, hardware prefetch happy.
* **Workers are CPU-only** — they hold a read-only view of a 4 K
  range inside the ring. They never call into libext2fs (so no GIL-
  like global state to worry about) and they never write to disk.
* **Writer is single-threaded** so `intervals_add` never contends and
  `recover_block_to_file` doesn't need its own lock. The rate of
  writer work is bounded by the rate of *hits*, which is sparse —
  even on 4 T the kernel reports thousands of leaf-magic hits, not
  millions. Single writer is plenty.

## Data flow / synchronization

* **Ringbuffer** is a fixed array of N `chunk_t` slots
  (`{buf, blk_start, blk_count, state}`) where state is one of
  `EMPTY → READING → READY → CLAIMING_<i> → DONE`.
  Reader transitions EMPTY → READING → READY via `compare_exchange`
  (no mutex). Workers atomically claim a READY slot.
  When all workers are done with a slot, it returns to EMPTY.
* **Hit queue** is a lock-free MPSC queue (one producer per worker,
  one consumer = writer). C11 atomic indexed ring; if a worker can't
  enqueue (writer is too slow), it spin-waits — but in practice the
  writer is many orders of magnitude faster than IO.
* **`recovered_intervals`** uses its existing `pthread_rwlock_t`.
  Workers `rdlock` for queries (in the early-skip pre-pass), writer
  `wrlock` for `intervals_add`. Readers won't block readers; the
  only contention is reader-vs-writer at most a few hundred times
  per scan.

## Per-segment checkpoint (resumability)

Aggressive scans take long enough that crash/interrupt recovery
matters. v5 already has a checkpoint json at
`<recover_dir>/.ext4recover_checkpoint.json`; we extend it:

```json
{
  "version": "0.5",
  "device": "/dev/vdb1",
  "mode": 8,
  "aggressive_segment_done": 47,
  "aggressive_segment_size_blocks": 262144,
  "files_recovered": 12,
  "journal_recovered": 7,
  "aggressive_recovered": 4,
  "intervals": [...]
}
```

After completing each 1 GB segment (256K blocks at 4 K), writer
flushes its outstanding files, dumps the current `intervals` array
into the json, and `fsync`s the file. On `--resume`, the reader
resumes from `aggressive_segment_done * aggressive_segment_size_blocks`
and the dedup interval set is preloaded.

This preserves dedup correctness across restarts: a file recovered
in segment 1 won't be re-dumped if its physical extents reappear
in segment 47.

## Determinism

The output of a parallel run **must** be byte-identical to the
single-threaded run on the same input. To guarantee this:

* The writer thread is the only writer, so output ordering depends
  only on the order in which it dequeues hits.
* The hit queue receives hits from workers in scan-order (each worker
  processes a block range that is a strict suffix of the reader's
  position). Workers enqueue with their `block_num` so the writer can
  optionally sort within a small window if exact ordering matters,
  but for `aggressive_<block>` filenames, ordering doesn't matter —
  each filename is unique.
* Tie-breaker for hits within the same chunk: workers process blocks
  inside their assigned range in ascending order, and only enqueue
  *after* the chunk's processing is complete. The writer drains
  one chunk at a time. This makes the per-chunk ordering identical
  to single-threaded.

## Test plan

* **Regression**: re-run T-DEDUP-2 with the parallel binary, confirm
  the output set matches the single-threaded baseline byte-for-byte.
  `diff -r` between the two recovery directories must be empty.
* **Performance**: measure 500 GB scan time on `/dev/vdb5`,
  expect ≤ 1 h (4 worker threads).
* **Resume**: kill `-9` mid-scan, restart with `--resume`, verify
  final output equals the uninterrupted run.
* **Stress**: run T-DEDUP-2 with 8 worker threads on a partition
  with thousands of synthetic leaf-headers (we'd need to fabricate
  this) to flush out any latent contention bug.

## Open questions

* Does `io_channel_read_blk64` from libext2fs expose the underlying
  fd in a way that keeps the bigalloc cluster mapping consistent?
  The cleanest alternative is just to do `pread64` on `device_fd`
  and ignore libext2fs entirely in the reader thread (we already do
  this in `recover_block_to_file`).
* Direct IO (`O_DIRECT`)? On VMs with virtio-blk it's usually
  slower than pagecache because the host caches anyway. Skip for
  now; revisit if scanning bare-metal NVMe.
* Should we keep the synchronous `--aggressive` path as a fallback
  for `--single-thread` debugging? Probably yes, behind
  `--no-parallel` flag.

## Files to be modified / added

```
improved/aggressive_scan_v5.c       # major rewrite of scan_for_extent_headers
improved/parallel_scan.c            # NEW reader/worker/writer threads
improved/parallel_scan.h            # NEW
improved/recovered_intervals.c      # the rwlock is already there
improved/utils_v5.c                 # extend save_checkpoint json
improved/Makefile_v5                # add parallel_scan.c, ensure -lpthread
```

Estimated work: ~600 LOC, 1–2 days of development + testing.

---

## 2026-06-03 update: post-implementation measurement

We implemented this design and measured it on the real test environment. **The parallel pipeline did not deliver any speedup on a single physical disk.** This section records exactly what happened so future maintainers don't repeat the same mistake.

### What was built

* `improved/parallel_scan.c` (~370 LoC): a chunked reader → worker pool → ordered writer pipeline as designed above.
* Integration in `aggressive_scan_v5.c`: when `ctx->use_parallel` is set, `scan_for_extent_headers()` dispatches to the parallel path instead of the serial per-block loop.
* New CLI: `--parallel` (default OFF), `--workers N` (default `ncpu-1`).

### Correctness check (T-PAR-1, 40 GB partition)

Two 600 MB / 300 MB multi-level-extent files were written, deleted, then recovered by aggressive scan in both modes:

```
byte-for-byte diff (parallel vs serial output dirs): IDENTICAL
md5 of recovered aggressive_33806 (600 MB): e2eb343cc1b12f0ae97700450c84c0ff  ✓ matches manifest big1
md5 of recovered aggressive_33805 (300 MB): 59a228a67d8bce65724decec0f95d0b0  ✓ matches manifest big2
```

So the implementation is functionally correct; the ordered-writer design successfully produces deterministic, byte-identical output to the serial path.

### Throughput measurement (T-PAR-2, 300 GB partition)

| Path | Time | Effective throughput | Notes |
|------|------|----------------------|-------|
| Raw `dd if=/dev/vdb6 of=/dev/null bs=8M iflag=direct count=2560` | 80.3 s for 20 GiB | **267 MB/s** | physical ceiling of this cloud disk |
| Serial aggressive scan (Phase 2 binary, `dedup_v1`) | ~20 min for 300 GB | **~245 MB/s** | already at **92 % of physical ceiling** |
| Parallel aggressive scan, 7 workers | killed at 33 min after ~150 GB scanned | **~77 MB/s avg** | **negative result** — slower than serial |

The 300 GB run was terminated at 33 min because it was clearly losing to serial. Even at the half-way point the per-chunk progress log showed `writer keeps up with reader, no backlog`, confirming that the CPU side is not the bottleneck and adding workers does not help.

### Why the design didn't pay off

The original assumption was *"4 KB pread syscall overhead is the bottleneck, batching to 64 MB will free a lot of headroom."* That assumption was wrong:

1. **The serial path was already IO-bound, not syscall-bound.** Linux's readahead and page cache turn many small sequential 4 KB reads into a small number of large device-level fetches, so the per-syscall cost is amortized away. The ratio `serial-throughput / raw-throughput = 92 %` proves the serial loop is using nearly all the available bandwidth.
2. **More worker threads cannot create bandwidth.** On a single physical device, sequential reads are limited by the device's max read MB/s. CPU-side magic verification takes microseconds per block on modern x86; it was never the limit.
3. **The pipeline added net overhead.** Two costs in particular:
   * The writer still does a per-hit `pread64()` to fetch the leaf block on each match (it can't reuse the worker's chunk buffer because by the time the writer runs, the buffer has been recycled). That re-read fights for the same disk's bandwidth.
   * Ring-buffer slot management + condition-variable signaling adds tens of µs per chunk; with 19 200 chunks that's measurable, but tiny vs the IO time. The bigger killer is the duplicate IO above.
4. **The disk does not benefit from concurrent random read.** Even if the writer's per-hit pread did not exist, kicking off N concurrent sequential streams on the same device would only convert one near-optimal sequential read pattern into N interleaved ones — strictly worse for both spinning rust and cloud-virtualized block devices.

### When would parallelization actually help?

The design is preserved (opt-in via `--parallel`) because there are real scenarios where it would pay off:

* **Multi-spindle striped array** (RAID0 / LVM striped / MD RAID with multiple physical members): each worker reads from a different physical spindle, total bandwidth scales with stripe count.
* **NVMe SSD with deep queue depth**: a single thread cannot saturate a modern NVMe (which wants QD=32+). Multiple workers issuing pread in parallel can hit ≥ 3 GB/s where one thread gets stuck around 1 GB/s.
* **Network-attached scratch (NFS / iSCSI / Ceph RBD)**: client-side concurrent IO is necessary to hide network latency.

For all of these, the existing pipeline is the right shape, and the determinism work was not wasted.

### What changed in this repo as a result

* Default of `g_ctx.use_parallel` flipped from 1 to 0 (see `ext4recover_v5.c`).
* CLI flag renamed semantically: was `--no-parallel` (opt-out), now `--parallel` (opt-in).
* New baseline snapshot: `improved/baselines/ext4recover_v5.c.parallel_optin`.
* Phase 3 status in `STATUS.md` reset to "implemented, disabled by default" with the throughput evidence above.

The verdict is unambiguous: **on a single physical disk the IO bandwidth IS the bottleneck. Future performance work should target IO patterns, not CPU concurrency.**
