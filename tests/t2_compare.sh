#!/bin/bash
# T2 journal对照: before_normal_fix vs after_t6_fix
set -u
. /mnt/work/testfw/lib_recover_test.sh

DEV=/dev/vdb6
MANI=$MANIFEST_DIR/t2_compare.mani

prepare_target $DEV "-b4096"
mkdir -p $TARGET_MNT/sf
rm -f "$MANI"
for sz in 4 16 64 100 200 256 500 1024 2048 4096; do
    dd if=/dev/urandom of=$TARGET_MNT/sf/f${sz}k bs=1024 count=$sz status=none
    sync
    record_file "$TARGET_MNT/sf/f${sz}k" "$MANI"
done
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
log "rm sf"
rm -f $TARGET_MNT/sf/f*
sync
detach_target

V_OLD=/home/ubuntu/ext4_recover_improved/ext4recover_v5.bin.before_normal_fix_20260602_110302
V_NEW=/home/ubuntu/ext4_recover_improved/ext4recover_v5

for VBIN in $V_OLD $V_NEW; do
    NAME=$(basename $VBIN)
    RDIR=$RECOVER_BASE/t2cmp_${NAME}
    sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
    log ">>> $NAME --journal"
    sudo $VBIN --journal --dir "$RDIR" $DEV 2>&1 | grep -E 'Journal recovered|Total files|Filename map|Recovered' | head -15
    echo "  recovered files in $RDIR:"
    sudo ls -la "$RDIR" | head -15
    log ">>> verify $NAME"
    verify_recovery_baseline "$MANI" "$RDIR"
    echo
done
