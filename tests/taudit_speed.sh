#!/bin/bash
# T-AUDIT-SPEED: A/B speed test - dedup_v1 vs audit_v1 on same 2GB file.
set -uo pipefail
source /mnt/work/testfw/lib_recover_test.sh

DEV=/dev/vdb1   # same as T0a
# We assume T0a just ran and the partition is in the "deleted bigfile" state.
# If not, recreate.

EXPECT_MD5=$(cat /mnt/work/manifests/t0a_largefile.mani 2>/dev/null | awk -F'|' '{print $4; exit}')
echo "expected md5: $EXPECT_MD5"

for B in dedup_v1 audit_v1; do
  RDIR=/mnt/work/recover_out/taudit_$B
  sudo rm -rf $RDIR; sudo mkdir -p $RDIR
  echo
  echo "== $B =="
  T0=$(date +%s.%N)
  sudo /home/ubuntu/ext4_recover_improved/ext4recover_v5.bin.$B --normal --dir $RDIR $DEV 2>&1 \
    | tail -8
  T1=$(date +%s.%N)
  ELAPSED=$(echo "$T1 - $T0" | bc)
  
  # Find recovered file
  F=$(sudo find $RDIR -name '12_file' -o -name 'bigfile' -o -name '*' -type f 2>/dev/null | head -1)
  SZ=$(sudo stat -c %s $F 2>/dev/null || echo 0)
  MD5=""
  if [[ -n "$F" && "$SZ" -gt 0 ]]; then
    # Truncate to expected size if needed before md5
    EXPECTED_SIZE=2147483648
    if [[ "$SZ" -gt "$EXPECTED_SIZE" ]]; then
      sudo truncate -s $EXPECTED_SIZE $F
    fi
    MD5=$(sudo md5sum $F | awk '{print $1}')
  fi
  printf "  binary=%-12s time=%.2fs  recovered_size=%d  md5=%s  match=%s\n" \
    "$B" "$ELAPSED" "$SZ" "$MD5" \
    "$([[ "$MD5" == "$EXPECT_MD5" ]] && echo YES || echo NO)"
done
