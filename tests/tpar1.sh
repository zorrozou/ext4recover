#!/bin/bash
# T-PAR-1: parallel aggressive scan correctness regression
# On a 40G partition (vdb7), write several files of varying sizes, delete them,
# then run aggressive scan BOTH parallel and serial (--no-parallel).
# Outputs must be byte-for-byte identical (diff -r empty), and every recovered
# file must md5-match its manifest entry.
set -u
. /mnt/work/testfw/lib_recover_test.sh

V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb7
TEST=tpar1
MANI=$MANIFEST_DIR/$TEST.mani
RDIR_PAR=$RECOVER_BASE/${TEST}_parallel
RDIR_SER=$RECOVER_BASE/${TEST}_serial
sudo rm -rf "$RDIR_PAR" "$RDIR_SER"; mkdir -p "$RDIR_PAR" "$RDIR_SER"
rm -f "$MANI"

echo "##################### T-PAR-1: parallel vs serial aggressive #####################"
prepare_target $DEV "-b4096"

log "step1: write test files (mix of sizes to create multi-level + single extents)"
# a big file (multi-level extent), two medium, several small
sudo dd if=/dev/urandom of=$TARGET_MNT/big1 bs=1M count=600 status=none
sudo dd if=/dev/urandom of=$TARGET_MNT/big2 bs=1M count=300 status=none
sudo dd if=/dev/urandom of=$TARGET_MNT/med1 bs=1M count=50  status=none
sudo dd if=/dev/urandom of=$TARGET_MNT/med2 bs=1M count=20  status=none
sync
for f in big1 big2 med1 med2; do
    record_file "$TARGET_MNT/$f" "$MANI"
    echo "  $f: $(sudo filefrag $TARGET_MNT/$f | awk '{print $2,$3}')"
done

log "step2: rm + sync"
sudo rm $TARGET_MNT/big1 $TARGET_MNT/big2 $TARGET_MNT/med1 $TARGET_MNT/med2
sync
detach_target

log ">>> A) PARALLEL aggressive scan (default, ncpu-1 workers)"
/usr/bin/time -v sudo $V5 --aggressive --dir "$RDIR_PAR" $DEV \
    > /mnt/work/logs/${TEST}_parallel.log 2>&1
PAR_RC=$?
log "   parallel done rc=$PAR_RC, elapsed: $(grep 'wall clock' /mnt/work/logs/${TEST}_parallel.log 2>/dev/null | sed 's/.*): //')"

log ">>> B) SERIAL aggressive scan (--no-parallel)"
/usr/bin/time -v sudo $V5 --aggressive --no-parallel --dir "$RDIR_SER" $DEV \
    > /mnt/work/logs/${TEST}_serial.log 2>&1
SER_RC=$?
log "   serial done rc=$SER_RC, elapsed: $(grep 'wall clock' /mnt/work/logs/${TEST}_serial.log 2>/dev/null | sed 's/.*): //')"

echo
log "=== compare file listings ==="
echo "PARALLEL products:"
sudo ls -la "$RDIR_PAR" | grep -v '^total\|^d' | awk '{print $5, $9}'
echo "SERIAL products:"
sudo ls -la "$RDIR_SER" | grep -v '^total\|^d' | awk '{print $5, $9}'

echo
log "=== byte-for-byte diff (parallel vs serial) ==="
if sudo diff -r "$RDIR_PAR" "$RDIR_SER" > /mnt/work/logs/${TEST}_diff.txt 2>&1; then
    echo "  IDENTICAL: parallel and serial outputs match byte-for-byte ✓"
else
    echo "  MISMATCH! see ${TEST}_diff.txt:"
    head -20 /mnt/work/logs/${TEST}_diff.txt
fi

echo
log "=== md5 verify (parallel output vs original manifest) ==="
verify_recovery_baseline "$MANI" "$RDIR_PAR"

echo
log "=== throughput summary ==="
echo "parallel: $(grep 'wall clock' /mnt/work/logs/${TEST}_parallel.log | sed 's/.*): //')"
echo "serial:   $(grep 'wall clock' /mnt/work/logs/${TEST}_serial.log | sed 's/.*): //')"
