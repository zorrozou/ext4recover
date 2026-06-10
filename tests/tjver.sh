#!/bin/bash
# tjver.sh - journal version-selection validation (A2 final form)
#
# Two adversarial scenarios for picking among multiple journal copies
# of the same inode:
#
#  X (audit-B2): write BIG -> truncate to 0 -> write SMALL -> rm.
#     The correct recovery is the SMALL (newest live) content.
#     A naive "most data wins" would wrongly resurrect BIG.
#
#  Y (mid-truncate artifacts): write BIG (multi-level) -> rm.
#     Deleting journals shrinking mid-truncate inode states at HIGHER
#     seq than the full live copy. A naive "newest seq wins" recovers
#     a truncated file. Correct recovery is the FULL content.
#
# Usage: tjver.sh <binary>
source "$(dirname "$0")/lib_recover_test.sh"

BIN="${1:?usage: $0 <binary>}"
DEV=/dev/vdb4
TEST=tjver

echo "##################### T-JVER #####################"
prepare_target "$DEV" "-b4096 -N 262144"

# --- Scenario X file ---
dd if=/dev/urandom of=$TARGET_MNT/xfile bs=1M count=512 status=none
sync
truncate -s 0 $TARGET_MNT/xfile
dd if=/dev/urandom of=$TARGET_MNT/xfile bs=1M count=8 status=none
sync
X_MD5=$(md5sum $TARGET_MNT/xfile | awk '{print $1}')
X_SIZE=$(stat -c %s $TARGET_MNT/xfile)
X_INO=$(stat -c %i $TARGET_MNT/xfile)

# --- Scenario Y file ---
dd if=/dev/urandom of=$TARGET_MNT/yfile bs=1M count=1024 status=none
sync
Y_MD5=$(md5sum $TARGET_MNT/yfile | awk '{print $1}')
Y_SIZE=$(stat -c %s $TARGET_MNT/yfile)
Y_INO=$(stat -c %i $TARGET_MNT/yfile)

log "X: ino=$X_INO size=$X_SIZE md5=$X_MD5 (expect SMALL/new content)"
log "Y: ino=$Y_INO size=$Y_SIZE md5=$Y_MD5 (expect FULL content)"

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
rm -f $TARGET_MNT/xfile $TARGET_MNT/yfile
detach_target

RDIR=$RECOVER_BASE/${TEST}
sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
sudo "$BIN" --journal --dir "$RDIR" "$DEV" >"$RDIR.log" 2>&1

check() {
    local label="$1" ino="$2" want_size="$3" want_md5="$4" name="$5"
    local f
    for f in "$RDIR/$name" "$RDIR/${ino}_file"; do
        [ -f "$f" ] && break
    done
    if [ ! -f "$f" ]; then echo "$label: MISS (no file)"; return 1; fi
    local got_size got_md5
    got_size=$(sudo stat -c %s "$f")
    got_md5=$(sudo head -c "$want_size" "$f" | md5sum | awk '{print $1}')
    if [ "$got_md5" = "$want_md5" ] && [ "$got_size" -ge "$want_size" ]; then
        echo "$label: OK ($f size=$got_size)"
        return 0
    fi
    echo "$label: FAIL ($f size=$got_size want=$want_size md5=$got_md5 want=$want_md5)"
    return 1
}

RC=0
check "X (newest-live wins)" "$X_INO" "$X_SIZE" "$X_MD5" xfile || RC=1
check "Y (full beats mid-truncate)" "$Y_INO" "$Y_SIZE" "$Y_MD5" yfile || RC=1

if [ $RC -eq 0 ]; then echo "T-JVER: PASS"; else echo "T-JVER: FAIL"; fi
exit $RC
