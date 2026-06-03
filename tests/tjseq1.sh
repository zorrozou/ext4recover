#!/bin/bash
# T-JSEQ-1: verify Phase 5 picks the newest jbd2 transaction-seq version,
# NOT the largest-size version.
#
# Setup: write the same file in 3 versions (V1=32MB 0xAA, V2=128MB 0xBB,
# V3=64MB 0xCC) with sync between each, then delete. Run v5 journal.
# Expected (new): V3 (newest seq) is recovered.
# Old behavior (size-largest): would recover V2 (128MB).
set -uo pipefail
source /mnt/work/testfw/lib_recover_test.sh

TEST=tjseq1
DEV=/dev/vdb7
RDIR=$WORK/recover_out/${TEST}_journal
MANI=$WORK/manifests/${TEST}.mani

echo "=== wiping $DEV ==="
sudo umount $DEV 2>/dev/null || true
sudo umount $TARGET_MNT 2>/dev/null || true
sudo dd if=/dev/zero of=$DEV bs=1M count=2048 status=none

prepare_target "$DEV"
MNT=$TARGET_MNT
sudo rm -rf $RDIR && sudo mkdir -p $RDIR
: > $MANI

mkbuf() {
  # mkbuf <out> <fillbyte> <MB>
  local out=$1 fb=$2 mb=$3
  sudo dd if=/dev/zero bs=1M count=$mb status=none | tr '\000' "\\$(printf %o $fb)" | sudo tee $out > /dev/null
}

# V1: 32 MB of 0xAA
echo "=== writing V1 (32MB of 0xAA) ==="
mkbuf $MNT/target 0xAA 32
sudo sync
V1_MD5=$(sudo md5sum $MNT/target | awk '{print $1}')
V1_SIZE=$(sudo stat -c %s $MNT/target)
echo "V1: size=$V1_SIZE md5=$V1_MD5"

# V2: truncate then rewrite as 128 MB of 0xBB
echo "=== writing V2 (128MB of 0xBB) ==="
sudo truncate -s 0 $MNT/target
sudo sync
mkbuf $MNT/target 0xBB 128
sudo sync
V2_MD5=$(sudo md5sum $MNT/target | awk '{print $1}')
V2_SIZE=$(sudo stat -c %s $MNT/target)
echo "V2: size=$V2_SIZE md5=$V2_MD5"

# V3: truncate then rewrite as 64 MB of 0xCC (this is the NEWEST version)
echo "=== writing V3 (64MB of 0xCC, NEWEST) ==="
sudo truncate -s 0 $MNT/target
sudo sync
mkbuf $MNT/target 0xCC 64
sudo sync
V3_MD5=$(sudo md5sum $MNT/target | awk '{print $1}')
V3_SIZE=$(sudo stat -c %s $MNT/target)
INO=$(sudo stat -c %i $MNT/target)
echo "V3 (expected): ino=$INO size=$V3_SIZE md5=$V3_MD5"
echo "$INO|target|$V3_SIZE|$V3_MD5" > $MANI

# 4) delete and umount
echo "=== deleting + umount ==="
sudo rm $MNT/target
sudo sync
detach_target

# 5) journal scan
echo "=== v5 --journal ==="
sudo /home/ubuntu/ext4_recover_improved/ext4recover_v5 --journal --verbose --dir $RDIR $DEV 2>&1 \
  | grep -E "Found deleted inode|seq=|Journal recovered|Total files|Recovered.* files from|Dedup" | head -40

echo "=== produced files ==="
sudo ls -la $RDIR

# Find a file matching ino $INO or named 'target'
shopt -s nullglob
RES=""
RES_MD5=""
RES_SIZE=""
for f in $RDIR/${INO}_file $RDIR/target $RDIR/*; do
  [[ -e "$f" ]] || continue
  M=$(sudo md5sum "$f" | awk '{print $1}')
  S=$(sudo stat -c %s "$f")
  printf "  candidate: %-50s size=%-12s md5=%s\n" "$f" "$S" "$M"
  if [[ -z "$RES" ]]; then
    RES="$f"
    RES_MD5="$M"
    RES_SIZE="$S"
  fi
done

echo
echo "=== verdict ==="
echo "expected V3: size=$V3_SIZE md5=$V3_MD5"
echo "got        : size=$RES_SIZE md5=$RES_MD5  ($RES)"

if [[ "$RES_MD5" == "$V3_MD5" ]]; then
  echo "RESULT: T-JSEQ-1 PASS (Phase 5 picked newest-seq version V3, not size-largest V2)"
  exit 0
elif [[ "$RES_MD5" == "$V2_MD5" ]]; then
  echo "RESULT: T-JSEQ-1 FAIL (still picking size-largest V2 instead of newest V3)"
  exit 1
elif [[ "$RES_MD5" == "$V1_MD5" ]]; then
  echo "RESULT: T-JSEQ-1 SURPRISING (picked oldest V1)"
  exit 1
else
  echo "RESULT: T-JSEQ-1 INCONCLUSIVE (got something not matching any version)"
  exit 2
fi
