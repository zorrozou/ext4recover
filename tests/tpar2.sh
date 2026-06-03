#!/bin/bash
# T-PAR-2: parallel throughput on a larger partition (vdb6 300G)
# Measures wall-clock for parallel aggressive scan; compares against the
# known serial baseline (~245 MB/s from earlier T-DEDUP-2 on 500G).
set -u
. /mnt/work/testfw/lib_recover_test.sh

V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb6
TEST=tpar2
MANI=$MANIFEST_DIR/$TEST.mani
RDIR_PAR=$RECOVER_BASE/${TEST}_parallel
sudo rm -rf "$RDIR_PAR"; mkdir -p "$RDIR_PAR"
rm -f "$MANI"

echo "##################### T-PAR-2: parallel throughput on 300G #####################"
prepare_target $DEV "-b4096"
log "step1: write a 1GB multi-level extent file"
sudo dd if=/dev/urandom of=$TARGET_MNT/big bs=1M count=1024 status=none
sync
record_file "$TARGET_MNT/big" "$MANI"
echo "  big: $(sudo filefrag $TARGET_MNT/big | awk '{print $2,$3}')"
log "step2: rm + sync"
sudo rm $TARGET_MNT/big
sync
detach_target

DEV_BYTES=$(sudo blockdev --getsize64 $DEV)
DEV_GB=$((DEV_BYTES / 1024 / 1024 / 1024))
log "device size: ${DEV_GB} GB"

log ">>> parallel aggressive scan, workers=$(($(nproc)-1))"
START=$(date +%s)
sudo $V5 --aggressive --dir "$RDIR_PAR" $DEV > /mnt/work/logs/${TEST}_par.log 2>&1
RC=$?
END=$(date +%s)
ELAPSED=$((END-START))
log "   done rc=$RC elapsed=${ELAPSED}s"
if [ "$ELAPSED" -gt 0 ]; then
    MBPS=$((DEV_GB * 1024 / ELAPSED))
    log "   throughput: ~${MBPS} MB/s (baseline serial was ~245 MB/s)"
fi

echo
log "=== recovered products ==="
sudo ls -la "$RDIR_PAR" | grep aggressive | awk '{print $5,$9}'
echo
log "=== md5 check vs manifest (big) ==="
EXP=$(awk -F'|' 'NR==1{print $3}' "$MANI")
echo "expected: $EXP"
F=$(sudo ls /mnt/work/recover_out/${TEST}_parallel/aggressive_* 2>/dev/null | head -1)
if [ -n "$F" ]; then
    GOT=$(sudo md5sum "$F" | awk '{print $1}')
    echo "got     : $GOT  ($F)"
    [ "$EXP" = "$GOT" ] && echo "  MD5 MATCH ✓" || echo "  MD5 MISMATCH ✗"
fi
