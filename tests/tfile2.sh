#!/bin/bash
# T-FILE-2 v2: synthetic test of Phase 4 walk_extent_tree.
set -uo pipefail
source /mnt/work/testfw/lib_recover_test.sh

TEST=tfile2
DEV=/dev/vdb7
RDIR=$WORK/recover_out/${TEST}_aggr
MANI=$WORK/manifests/${TEST}.mani

# 1) WIPE the partition fully so previous runs' leaf blocks don't interfere
echo "=== wiping $DEV (zeroing first 4 GB) ==="
sudo umount $DEV 2>/dev/null || true
sudo umount $TARGET_MNT 2>/dev/null || true
sudo dd if=/dev/zero of=$DEV bs=1M count=4096 status=none

prepare_target "$DEV"
MNT=$TARGET_MNT
sudo rm -rf $RDIR && sudo mkdir -p $RDIR
: > $MANI

# 2) write a target file with KNOWN content (so md5 is deterministic across runs)
echo "=== writing target file (64 MB urandom) ==="
sudo dd if=/dev/urandom of=$MNT/F bs=1M count=64 status=none
sudo sync
EXPECT_MD5=$(sudo md5sum $MNT/F | awk '{print $1}')
EXPECT_SIZE=$(sudo stat -c %s $MNT/F)
INO=$(sudo stat -c %i $MNT/F)
echo "F: ino=$INO size=$EXPECT_SIZE md5=$EXPECT_MD5" | tee -a $MANI

# 3) read the actual leaf-extent from inode via debugfs
echo "=== debugfs stat of inode $INO ==="
sudo debugfs -R "stat <$INO>" $DEV 2>/dev/null | tee /tmp/dbgstat
# parse: line like "(0):16384, (0-16383):34816-51199"
EXT_LINE=$(grep -oE '\([0-9-]+\):[0-9-]+' /tmp/dbgstat | tail -1)
echo "EXT_LINE: $EXT_LINE"
# range like "(0-16383):34816-51199"
RANGE=${EXT_LINE#*:}
PHYS_START=${RANGE%-*}
PHYS_END=${RANGE#*-}
LEN=$((PHYS_END - PHYS_START + 1))
EE_BLOCK=0
echo "phys_start=$PHYS_START phys_end=$PHYS_END len=$LEN ee_block=$EE_BLOCK"

# 4) delete F and detach
sudo rm $MNT/F
sudo sync
detach_target

# 5) Forge a depth=1 root block at block 1000000 pointing to the leaf.
#    But wait - PHYS_START is a DATA block, not an extent-header block.
#    For Phase 4 to work, we need an INDEX node whose ei_leaf points to
#    a *LEAF EXTENT HEADER* block. Since F was depth=0 (root in inode),
#    the only leaf-header block on disk that survives is none - F's root
#    was in inode_table, which got cleared.
#
#    So we need a different setup: forge BOTH a leaf and a root:
#      - At block 1000010: write a depth=0 leaf header with 1 extent
#        pointing to PHYS_START..PHYS_END (the actual data)
#      - At block 1000000: write a depth=1 root header with 1 idx
#        pointing to block 1000010

LEAF_BLK=1000010
ROOT_BLK=1000000

echo "=== forging leaf at block $LEAF_BLK and root at block $ROOT_BLK ==="
python3 - <<PYEOF
import struct
phys = $PHYS_START
ln   = $LEN
ee_block = 0

# Leaf: depth=0, 1 extent
leaf_hdr = struct.pack('<HHHHI', 0xF30A, 1, 340, 0, 0)
ee       = struct.pack('<IHHI', ee_block, ln, (phys >> 32) & 0xFFFF, phys & 0xFFFFFFFF)
leaf_buf = leaf_hdr + ee
leaf_buf += b'\x00' * (4096 - len(leaf_buf))
open('/tmp/forged_leaf.bin', 'wb').write(leaf_buf)

# Root: depth=1, 1 idx pointing at LEAF_BLK
leaf_blk = $LEAF_BLK
root_hdr = struct.pack('<HHHHI', 0xF30A, 1, 340, 1, 0)
idx      = struct.pack('<IIHH', ee_block, leaf_blk & 0xFFFFFFFF, (leaf_blk >> 32) & 0xFFFF, 0)
root_buf = root_hdr + idx
root_buf += b'\x00' * (4096 - len(root_buf))
open('/tmp/forged_root.bin', 'wb').write(root_buf)

print(f"forged: leaf->{phys}..{phys+ln-1} (len={ln}); root idx->leaf @ {leaf_blk}")
PYEOF

sudo dd if=/tmp/forged_leaf.bin of=$DEV bs=4096 count=1 seek=$LEAF_BLK conv=notrunc status=none
sudo dd if=/tmp/forged_root.bin of=$DEV bs=4096 count=1 seek=$ROOT_BLK conv=notrunc status=none
sudo sync

# 6) run aggressive scan with verbose to see Phase 4 path
echo "=== aggressive scan (verbose, grep tree-related) ==="
sudo /home/ubuntu/ext4_recover_improved/ext4recover_v5 --aggressive --verbose --dir $RDIR $DEV 2>&1 \
  | grep -E "tree walk|tree root|Reconstructed|Found orphaned|aggressive_tree|extent header at $ROOT_BLK|extent header at $LEAF_BLK" \
  | head -30

# 7) verify
echo "=== produced files ==="
sudo ls -la $RDIR

shopt -s nullglob
HIT_TREE=""
HIT_LEAF=""
for f in $RDIR/aggressive_tree_* $RDIR/aggressive_*; do
  [[ -e "$f" ]] || continue
  M=$(sudo md5sum "$f" | awk '{print $1}')
  S=$(sudo stat -c %s "$f")
  printf "  candidate: %s size=%d md5=%s\n" "$f" "$S" "$M"
  if [[ "$M" == "$EXPECT_MD5" ]]; then
    case "$f" in
      *aggressive_tree_*) HIT_TREE="$f" ;;
      *) HIT_LEAF="$f" ;;
    esac
    echo "  ==> MATCH on $f"
  fi
done

if [[ -n "$HIT_TREE" ]]; then
  echo "RESULT: T-FILE-2 PASS (Phase 4 tree path triggered; aggressive_tree_* md5 matches expected)"
  exit 0
fi
if [[ -n "$HIT_LEAF" ]]; then
  echo "RESULT: T-FILE-2 PARTIAL (file recovered via leaf path, not tree path)"
  exit 2
fi
echo "RESULT: T-FILE-2 FAIL (no md5 match)"
exit 1
