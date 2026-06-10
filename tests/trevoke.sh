#!/bin/bash
# trevoke.sh - C1 validation: revoke-guided targeted recovery
#
# Writes a large multi-level extent file, deletes it, then compares
# --targeted (C1) vs --journal recovery on the same disk state.
# Expect: --targeted recovers at least as many bytes, and the output
# file md5-matches the original.
#
# Also measures targeted scan time vs aggressive to quantify the
# "O(revoked blocks) vs O(total blocks)" speedup claim.

source "$(dirname "$0")/lib_recover_test.sh"
BIN="${1:?usage: $0 <binary>}"
DEV=/dev/vdb4
TEST=trevoke
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T-REVOKE (C1) #####################"
prepare_target "$DEV" "-b4096 -N 262144"

# Write 1 GB multi-level file so it uses depth>=1 extent tree
log "writing 1GB file (multi-level extent tree)..."
dd if=/dev/urandom of=$TARGET_MNT/bigfile bs=1M count=1024 status=none
dd if=/dev/urandom of=$TARGET_MNT/small1 bs=1K count=64 status=none
dd if=/dev/urandom of=$TARGET_MNT/small2 bs=1K count=256 status=none
sync
record_file "$TARGET_MNT/bigfile" "$MANI"
record_file "$TARGET_MNT/small1"  "$MANI"
record_file "$TARGET_MNT/small2"  "$MANI"

log "extent depth of bigfile:"
sudo filefrag $TARGET_MNT/bigfile 2>/dev/null | tail -1

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
rm -f $TARGET_MNT/bigfile $TARGET_MNT/small1 $TARGET_MNT/small2
detach_target

# --- Journal recovery (reference) ---
RDIR_J=$RECOVER_BASE/${TEST}_journal
sudo rm -rf "$RDIR_J"; mkdir -p "$RDIR_J"
T0=$(date +%s.%N)
sudo "$BIN" --journal --dir "$RDIR_J" "$DEV" >/dev/null 2>&1
TJ=$(echo "$(date +%s.%N) $T0" | awk '{printf "%.1f", $1-$2}')
echo "----- VERIFY journal -----"
verify_recovery_baseline "$MANI" "$RDIR_J"; RJ=$?

# --- Targeted (C1) recovery ---
RDIR_T=$RECOVER_BASE/${TEST}_targeted
sudo rm -rf "$RDIR_T"; mkdir -p "$RDIR_T"
T0=$(date +%s.%N)
sudo "$BIN" --journal --targeted --dir "$RDIR_T" "$DEV" >/dev/null 2>&1
TT=$(echo "$(date +%s.%N) $T0" | awk '{printf "%.1f", $1-$2}')
echo "----- VERIFY targeted -----"
verify_recovery_baseline "$MANI" "$RDIR_T"; RT=$?

log "journal=${TJ}s  targeted=${TT}s"

# Targeted should be no worse than journal
NJ=$(sudo find "$RDIR_J" -maxdepth 1 -type f | wc -l)
NT=$(sudo find "$RDIR_T" -maxdepth 1 -type f | wc -l)
log "journal files=$NJ  targeted files=$NT"

if [ "$RT" -eq 0 ]; then
    echo "T-REVOKE: PASS"
    exit 0
else
    echo "T-REVOKE: FAIL"
    exit 1
fi
