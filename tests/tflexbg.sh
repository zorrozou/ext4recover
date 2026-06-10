#!/bin/bash
# tflexbg.sh - A1 validation: journal recovery on flex_bg disks
#
# The pre-A1 calc_inode_from_block() assumed inode tables live inside
# their owning block group. Under flex_bg (mkfs default), the tables
# of a 16-group flex group are packed into the leader group, so only
# inodes whose group is a flex LEADER (k=0) were ever recovered from
# the journal; inodes in member groups (k>0) were silently skipped.
#
# This test forces files into many different block groups (the Orlov
# allocator spreads top-level directories across groups), deletes
# them, and compares --journal recovery between two binaries.
#
# Usage: tflexbg.sh <binary_A=old> <binary_B=A1>
# Expect: B recovers a strict superset; A misses non-leader groups.

source "$(dirname "$0")/lib_recover_test.sh"

BIN_A="${1:?usage: $0 <binary_A> <binary_B>}"
BIN_B="${2:?need second binary}"
DEV=/dev/vdb4
TEST=tflexbg
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T-FLEXBG (A1) #####################"

prepare_target "$DEV" "-b4096"
IPG=$(sudo dumpe2fs -h $DEV 2>/dev/null | awk -F: '/Inodes per group/{gsub(/ /,"");print $2}')
log "flex_bg fs ready, inodes_per_group=$IPG"

# 1. Spread 48 top-level dirs (Orlov scatters them across groups),
#    one 256K file in each. Record inode + owning group.
log "writing 48 files spread across block groups..."
for i in $(seq -w 1 48); do
    mkdir "$TARGET_MNT/dir$i"
    dd if=/dev/urandom of="$TARGET_MNT/dir$i/f$i" bs=64K count=4 status=none
done
sync
for i in $(seq -w 1 48); do
    record_file "$TARGET_MNT/dir$i/f$i" "$MANI"
done

# Group distribution evidence
log "inode group distribution (group: count):"
for i in $(seq -w 1 48); do
    ino=$(stat -c %i "$TARGET_MNT/dir$i/f$i")
    echo $(( (ino - 1) / IPG ))
done | sort -n | uniq -c | awk '{printf "    group %s: %s files\n", $2, $1}'

# 2. Delete everything, unmount
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
rm -rf "$TARGET_MNT"/dir*
detach_target

# 3. Run both binaries (--journal) on the same disk state
run_one() {
    local bin="$1" tag="$2"
    local rdir=$RECOVER_BASE/${TEST}_$tag
    sudo rm -rf "$rdir"; mkdir -p "$rdir"
    sudo "$bin" --journal --dir "$rdir" "$DEV" >"$rdir.log" 2>&1
    echo "$rdir"
}
RDIR_A=$(run_one "$BIN_A" A)
RDIR_B=$(run_one "$BIN_B" B)

echo "----- VERIFY A ($(basename $BIN_A)) -----"
verify_recovery_baseline "$MANI" "$RDIR_A"; RA=$?
echo "----- VERIFY B ($(basename $BIN_B)) -----"
verify_recovery_baseline "$MANI" "$RDIR_B"; RB=$?

NA=$(sudo find "$RDIR_A" -maxdepth 1 -type f ! -name ".*" | wc -l)
NB=$(sudo find "$RDIR_B" -maxdepth 1 -type f ! -name ".*" | wc -l)
echo "A recovered files: $NA   B recovered files: $NB"

if [ "$NB" -ge "$NA" ] && [ "$RB" -eq 0 ]; then
    echo "T-FLEXBG: PASS (B >= A and B verifies)"
    exit 0
else
    echo "T-FLEXBG: FAIL"
    exit 1
fi
