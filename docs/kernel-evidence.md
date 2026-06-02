# Kernel Evidence — Why this whole project works

The original `ext4recover` and our improvements both rely on a
specific behavior of the Linux kernel ext4 deletion path. This file
pulls the **actual lines of code** from the upstream Linux source
(`fs/ext4/extents.c`, kernel v7.0-rc7 family) that make this behavior
true, so future readers don't have to take the README's word for it.

## TL;DR

| File type at delete time | Kernel function | What happens to data on disk | Recoverable by `--normal`? | Recoverable by `--journal`? |
|---|---|---|---|---|
| Multi-level (depth ≥ 1, root has ≥ 1 idx slots) | `ext4_ext_rm_idx` → `ext4_free_blocks(METADATA \| FORGET)` | Leaf data physically intact | ✅ yes | ✅ yes |
| Single-extent (depth = 0, all extents in inode) | `ext4_ext_rm_leaf` → `store_pblock(0); ee_len=0` on the in-inode extent | **Inode-internal extent zeroed at flush time** | ❌ no | ✅ if jbd2 still holds pre-delete inode copy |

## Multi-level case — `ext4_ext_rm_idx`

```c
/* fs/ext4/extents.c, in ext4_ext_rm_idx */
err = ext4_free_blocks(handle, inode, NULL, leaf, 1,
                       EXT4_FREE_BLOCKS_METADATA |
                       EXT4_FREE_BLOCKS_FORGET);
```

`EXT4_FREE_BLOCKS_FORGET` causes `__ext4_forget()` to discard the
in-memory buffer modifications instead of journaling them. The
on-disk content of the leaf block is never overwritten with zeros.
The buffer is just "forgotten" from the perspective of the
filesystem's metadata accounting.

Then the parent header is updated:

```c
le16_add_cpu(&path->p_hdr->eh_entries, -1);
err = ext4_ext_dirty(handle, inode, path);
```

Net effect: the parent's `eh_entries` decrements (potentially to 0
for the root in the inode), but **the leaf block on disk still holds
its original `ext4_extent` array**. This is the entire premise that
`ext4recover` relies on — those leaves are recoverable by reading the
raw block device.

It also explains why the original program's filter is just
`ei_leaf > 1` (with a regular file + `links_count == 0`), without
trusting `eh_entries` — it knows entries gets decremented to zero.

## Single-extent case — `ext4_ext_rm_leaf`

When the removal range covers the entire leaf and the leaf is the
inode-internal one (depth=0), `ext4_ext_rm_leaf` zeroes the in-inode
extent before journaling:

```c
/* fs/ext4/extents.c, in ext4_ext_rm_leaf, around line 2738-2742 */
ext4_ext_store_pblock(ex, 0);
ex->ee_len = 0;
```

This **modifies the inode itself** (i_block[]) and then journals the
modified inode. When the inode is flushed, the on-disk inode no longer
has any extent information for those blocks. The `links_count==0`
plus the cleared extent table together render the inode unrecoverable
through normal extent walking.

Only jbd2's transaction log can save these files: the *previous*
version of this inode (with intact extent), if it lives in a
descriptor block whose physical journal range hasn't been overwritten,
can be retrieved by scanning the journal. This is exactly what
`improved/journal_recovery_v5.c` does.

## Header layout we depend on

```c
/* fs/ext4/ext4_extents.h */
#define EXT4_EXT_MAGIC          0xf30a
#define EXT_INIT_MAX_LEN        32768          /* maximum length of an initialized extent */

struct ext4_extent_header {
    __le16 eh_magic;        /* magic number, 0xf30a */
    __le16 eh_entries;      /* number of valid entries */
    __le16 eh_max;          /* capacity of store in entries */
    __le16 eh_depth;        /* tree depth (0 = leaf) */
    __le32 eh_generation;
};

struct ext4_extent_idx {     /* internal nodes */
    __le32 ei_block;
    __le32 ei_leaf;          /* low 32 bits of physical block of next-level */
    __le16 ei_leaf_hi;       /* high 16 bits */
    __u16  ei_unused;
};

struct ext4_extent {         /* leaf nodes */
    __le32 ee_block;         /* logical block start */
    __le16 ee_len;           /* number of blocks; > 32768 = unwritten */
    __le16 ee_start_hi;
    __le32 ee_start;         /* physical block low 32 bits */
};
```

Key facts that drive the code:

* **`ee_len > 32768`** = unwritten / `fallocate`'d region. Real length
  is `ee_len - 32768`. Both `improved/ext4recover_v5.c` and
  `improved/journal_recovery_v5.c` decode this correctly. T4 is the
  real-disk evidence.
* **Physical block** = `(ee_start_hi << 32) | ee_start_lo`. This is
  always in *blocks*, never in bigalloc clusters, even when bigalloc
  is enabled. This was confirmed by reading
  `fs/ext4/ext4_extents.h::ext4_ext_pblock`:

  ```c
  static inline ext4_fsblk_t ext4_ext_pblock(struct ext4_extent *ex)
  {
      ext4_fsblk_t block;
      block = le32_to_cpu(ex->ee_start_lo);
      block |= ((ext4_fsblk_t) le16_to_cpu(ex->ee_start_hi) << 31) << 1;
      return block;
  }
  ```

  This corrected an early hypothesis that bigalloc would change extent
  addressing; it does not. Only `ee_len` units (clusters vs blocks)
  could in principle differ, but in practice ext4 extents in bigalloc
  use cluster-sized units that are still expressed as block counts in
  `ee_len`. T6 confirmed our `recover_block_to_file` correctness on a
  bigalloc filesystem.

## What we did NOT find

Two things that could in principle make recovery harder are confirmed
**absent** from current ext4 deletion code:

1. **No on-disk overwrite-with-zeros** for leaf blocks. We grep'd
   `extents.c` for `memset(... 0 ...)` calls in the rm path and found
   none that touch the leaf block content prior to free.
2. **No metadata checksum invalidation that prevents reading.** The
   `ext4_extent_tail::et_checksum` at the end of leaf blocks is just
   integrity tracking — `ext4recover` ignores it because we trust the
   data even when csum no longer matches.

## Where this evidence is in our code paths

| Kernel insight | Code path that exploits it |
|----------------|----------------------------|
| `rm_idx` + FORGET preserves leaves | `original/src/ext4recover.c::extent_tree_travel`, `improved/ext4recover_v5.c::dump_leaf_extent` |
| `eh_entries` may be 0 even when slots are valid | `improved/ext4recover_v5.c::recover_from_extent_tree` synthesizes count from `ei_leaf > 1` |
| jbd2 retains pre-delete inode copies for some window | `improved/journal_recovery_v5.c` brute-force scans all journal blocks |
| `EXT_INIT_MAX_LEN = 32768` boundary for unwritten extents | `improved/ext4recover_v5.c::dump_leaf_extent`, `improved/journal_recovery_v5.c` |
| Extent physical address always in blocks | `improved/ext4recover_v5.c::recover_block_to_file` (`phys_block = start`) |
