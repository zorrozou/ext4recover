#!/bin/bash
# T-JSEQ-AB: strict A/B/C compare on T2-style scenario across 3 binaries.
# Same disk state, 3 different binaries -> compare recovery count.
set -uo pipefail
source /mnt/work/testfw/lib_recover_test.sh

DEV=/dev/vdb7
SIZES=(4k 16k 64k 100k 200k 256k 500k 1024k 2048k 4096k)
BINS=(dedup_v1 tree_v1 jseq_v1)

# 1) wipe + mkfs + write + rm
sudo umount $DEV 2>/dev/null || true
sudo umount $TARGET_MNT 2>/dev/null || true
sudo dd if=/dev/zero of=$DEV bs=1M count=2048 status=none
prepare_target "$DEV"
MNT=$TARGET_MNT

declare -A ORIG_MD5
for s in "${SIZES[@]}"; do
  sudo dd if=/dev/urandom of=$MNT/f$s bs=$s count=1 status=none
  ORIG_MD5[$s]=$(sudo md5sum $MNT/f$s | awk '{print $1}')
done
sudo sync
for s in "${SIZES[@]}"; do sudo rm $MNT/f$s; done
sudo sync
detach_target

# 2) snapshot the partition into an image so each binary gets the SAME state
echo "=== snapshotting $DEV to /mnt/work/tjseqab.img (40G) ==="
sudo dd if=$DEV of=/mnt/work/tjseqab.img bs=1M count=40960 status=none
echo "snapshot size: $(sudo stat -c %s /mnt/work/tjseqab.img)"

# 3) run each binary against same snapshot
for B in "${BINS[@]}"; do
  echo
  echo "================== RUN binary=$B =================="
  RDIR=/mnt/work/recover_out/tjseqab_$B
  sudo rm -rf $RDIR; sudo mkdir -p $RDIR
  # restore image to a loop device
  LOOP=$(sudo losetup --find --show /mnt/work/tjseqab.img)
  echo "loop=$LOOP"
  sudo /home/ubuntu/ext4_recover_improved/ext4recover_v5.bin.$B --journal --dir $RDIR $LOOP 2>&1 \
    | grep -E "Journal recovered|Recovered .* files from|Total files|Dedup" | head -10
  sudo losetup -d $LOOP
  N=$(sudo ls $RDIR 2>/dev/null | wc -l)
  echo "files in $RDIR: $N"
  HIT=0
  for s in "${SIZES[@]}"; do
    for f in $RDIR/f$s $RDIR/*; do
      [[ -e "$f" ]] || continue
      M=$(sudo md5sum "$f" 2>/dev/null | awk '{print $1}')
      if [[ "$M" == "${ORIG_MD5[$s]}" ]]; then
        HIT=$((HIT+1))
        break
      fi
    done
  done
  echo "  $B: md5-matching files = $HIT / ${#SIZES[@]}"
done

echo
echo "=== A/B/C summary above ==="
