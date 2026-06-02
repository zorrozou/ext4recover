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
