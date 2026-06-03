#!/bin/bash
# T-FILE-1: verify Phase 4 file-level reconstruction in aggressive scan.
set -uo pipefail
source /mnt/work/testfw/lib_recover_test.sh

TEST=tfile1
DEV=/dev/vdb7
RDIR=$WORK/recover_out/${TEST}_aggr
MANI=$WORK/manifests/${TEST}.mani

prepare_target "$DEV"
MNT=$TARGET_MNT
sudo rm -rf $RDIR && sudo mkdir -p $RDIR
: > $MANI

echo "=== priming fragmentation noise ==="
for i in $(seq 1 80); do
  sudo dd if=/dev/urandom of=$MNT/noise_$i bs=128K count=4 status=none
done
sudo sync
for i in $(seq 2 2 80); do sudo rm $MNT/noise_$i; done
sudo sync

echo "=== writing target big file (1GB in 4 chunks with sync) ==="
for n in $(seq 0 3); do
  sudo dd if=/dev/urandom of=$MNT/big bs=1M count=256 seek=$((n*256)) conv=notrunc status=none
  sudo sync
done
sudo sync

EXPECT_MD5=$(sudo md5sum $MNT/big | awk '{print $1}')
EXPECT_SIZE=$(sudo stat -c %s $MNT/big)
INO=$(sudo stat -c %i $MNT/big)
echo "$INO|big|$EXPECT_SIZE|$EXPECT_MD5" > $MANI
echo "=== filefrag (before delete) ==="
sudo filefrag -v $MNT/big | head -25
EXT_COUNT=$(sudo filefrag $MNT/big | sed -n 's/.* \([0-9]\+\) extents found/\1/p')
echo "extent count = $EXT_COUNT"

echo "=== deleting + umount ==="
sudo rm $MNT/big
sudo rm $MNT/noise_* 2>/dev/null || true
detach_target

echo "=== aggressive scan ==="
sudo /home/ubuntu/ext4_recover_improved/ext4recover_v5 --aggressive --dir $RDIR $DEV 2>&1 | tail -25

echo "=== produced files ==="
sudo ls -la $RDIR | head -30

HIT=""
shopt -s nullglob
for f in $RDIR/aggressive_tree_* $RDIR/aggressive_*; do
  [[ -e "$f" ]] || continue
  M=$(sudo md5sum "$f" | awk '{print $1}')
  S=$(sudo stat -c %s "$f")
  printf "  candidate: %s size=%d md5=%s\n" "$f" "$S" "$M"
  if [[ "$M" == "$EXPECT_MD5" ]]; then
    HIT="$f"
    echo "  ==> MATCH on $f"
  fi
done

if [[ -n "$HIT" ]]; then
  echo "RESULT: T-FILE-1 PASS (1 complete file recovered)"
  exit 0
else
  echo "RESULT: T-FILE-1 FAIL (no candidate matches expected md5 $EXPECT_MD5)"
  exit 1
fi
