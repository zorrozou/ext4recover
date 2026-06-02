# Test Environment

## Hardware / VM

* Tencent Cloud VM, public IP 101.33.207.161
* OS: Ubuntu 24.04 LTS
* Kernel: 6.8 family
* `/dev/vda` 512 GB — system disk (NEVER touched by tests)
* `/dev/vdb` 10 TB — primary test target (partitioned, see below)
* `/dev/vdc` 10 TB — work disk (mounted as `/mnt/work`, holds
  recovery output / manifests / logs / images)

## `/dev/vdb` partition layout

Created with `parted -s /dev/vdb mklabel gpt mkpart … ext4 …`:

| Partition | Size | mkfs options | Purpose |
|-----------|------|--------------|---------|
| `vdb1` | 4 TB | `-b4096 -m0 -F` | T0a / T1: large multi-level extent files |
| `vdb2` | 2 TB | `-b4096 -m0 -F` | small file fleet (currently unused after T0b experiments confirmed mballoc resists fragmentation) |
| `vdb3` | 500 GB | `-b1024 -m0 -F` | non-4K block size (verifies max_entries scaling) |
| `vdb4` | 500 GB | `-b4096 -m0 -F -J size=64` | small journal — for journal-wrap experiments (T5) |
| `vdb5` | 500 GB | `-b4096 -O bigalloc -C 65536 -m0 -F` | bigalloc cluster=64K (T6, T-DEDUP-2) |
| `vdb6` | 300 GB | `-b4096 -m0 -F` | T2 (single-extent small files), T3 (fragmentation attempts) |

Mount points:

```
/mnt/target   ← the partition under test, mounted on demand
/mnt/work     ← /dev/vdc, always mounted, holds recover_out/ logs/ manifests/
```

## Whitelist enforcement

`tests/lib_recover_test.sh` declares:

```sh
ALLOWED_DEVS="/dev/vdb1 /dev/vdb2 /dev/vdb3 /dev/vdb4 /dev/vdb5 /dev/vdb6 /dev/vdc"
```

`assert_allowed_dev` is called from `prepare_target` before any
`mkfs` or `mount` operation, and aborts with `[FATAL]` if the device
is not in the list. **Never disable this** — it is the only thing
preventing accidental destruction of `/dev/vda`.

## Required tools on the test box

```
sudo apt install -y e2fsprogs libext2fs-dev gcc make
```

The framework also needs `tmux`, `sshpass` (for orchestration from
the dev machine), and `parted` (for initial partition setup).

## Running tests

Each test script is self-contained and assumes the framework lib has
been sourced. Pattern:

```bash
nohup bash tests/t0a.sh > logs/t0a.log 2>&1 & disown
# ssh-resilient: nohup + disown so the test survives shell drops
```

Long-running tests (aggressive scans on full 4 TB partitions) can
take hours. Use a timeboxed wrapper like `tdedup2.sh` for hands-off
runs.

## Recovery output structure

```
/mnt/work/
├── recover_out/
│   ├── t0a_largefile_orig/      ← from original ext4recover
│   │   └── RECOVER/
│   │       └── 12_file
│   ├── t0a_largefile_v5normal/  ← from v5 --normal
│   │   └── 12_file
│   ├── tdedup1/
│   │   └── big
│   ├── tdedup2/
│   │   ├── big
│   │   ├── aggressive_524432
│   │   ├── aggressive_67141631
│   │   └── aggressive_67633136
│   └── ...
├── manifests/                   ← per-test (ino, size, md5, original-path)
│   ├── t0a_largefile.mani
│   ├── t6_bigalloc.mani
│   └── ...
└── logs/                        ← stdout/stderr of each test run
    ├── t0a_dedup.log
    └── ...
```

Manifest format: `<ino>|<size>|<md5>|<original_path>` per line,
one line per file recorded by `record_file` in the framework.
