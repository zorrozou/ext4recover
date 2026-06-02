#!/bin/bash
# T2: v5 journal 模式恢复单级 extent 小文件 (突破原程序天花板)
# 单级extent小文件被rm后,内核ext4_ext_rm_leaf会将inode内extent清零(已实证),
# 原程序+v5--normal必失败. 但journal里仍有删除前的inode副本,v5--journal应能救.
source /mnt/work/testfw/lib_recover_test.sh

verify_recovery_byname() {
    # 支持按 {ino}_file 或 真实文件名 查找
    local mani="$1" rdir="$2"
    local total=0 ok=0
    while IFS='|' read -r ino size md5 path; do
        total=$((total+1))
        local fname=$(basename "$path")
        local rf
        # 优先按真实文件名,fallback {ino}_file
        rf=$(sudo find "$rdir" -name "$fname" 2>/dev/null | head -1)
        [ -z "$rf" ] && rf=$(sudo find "$rdir" -name "${ino}_file" 2>/dev/null | head -1)
        if [ -z "$rf" ] || sudo test ! -f "$rf"; then
            echo "  MISS ino=$ino name=$fname"
            continue
        fi
        local rmd5=$(sudo head -c "$size" "$rf" 2>/dev/null | md5sum | awk '{print $1}')
        if [ "$rmd5" = "$md5" ]; then
            ok=$((ok+1))
            echo "  OK   ino=$ino name=$fname md5=$rmd5"
        else
            echo "  FAIL ino=$ino orig=$md5 got=$rmd5 name=$fname"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total"
}

ORIG=/home/ubuntu/ext4_recover/src/ext4recover
V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb6   # 用300G小分区,避免normal/aggressive扫太久
TEST=t2_journal_singleext
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T2 v5 journal 救单级extent小文件 #####################"

prepare_target "$DEV" "-b4096"

# 写10个不同大小的小文件,期望每个都是单extent
log "step1: writing 10 small files (different sizes)"
mkdir -p $TARGET_MNT/sf
for sz in 4 16 64 100 200 256 500 1024 2048 4096; do
    dd if=/dev/urandom of=$TARGET_MNT/sf/f${sz}k bs=1024 count=$sz status=none
    sync
    record_file "$TARGET_MNT/sf/f${sz}k" "$MANI"
    INO=$(stat -c %i $TARGET_MNT/sf/f${sz}k)
    EXT=$(sudo filefrag $TARGET_MNT/sf/f${sz}k 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
    log "  f${sz}k ino=$INO size=${sz}KB extents=$EXT"
done

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
log "step2: rm small files (single-extent: inode-internal extent will be zeroed)"
rm -f $TARGET_MNT/sf/f*
detach_target

# Test A: 原程序 - 期望全失败 (内核语义证明)
RDIR_ORIG=$RECOVER_BASE/${TEST}_orig
sudo rm -rf "$RDIR_ORIG"; mkdir -p "$RDIR_ORIG"
log ">>> A) ORIG ext4recover (expected: 0/10 - kernel semantic) ..."
( cd "$RDIR_ORIG" && sudo "$ORIG" "$DEV" 2>&1 | grep -vE 'Unknown code' | tail -5 )
log "ORIG verify:"
verify_recovery_byname "$MANI" "$RDIR_ORIG/RECOVER"

# Test B: v5 --normal - 期望也失败
RDIR_VN=$RECOVER_BASE/${TEST}_v5normal
sudo rm -rf "$RDIR_VN"; mkdir -p "$RDIR_VN"
log ">>> B) v5 --normal (expected: 0/10) ..."
sudo "$V5" --normal --dir "$RDIR_VN" "$DEV" 2>&1 | grep -E 'Total files|recovered:|failed' | head -5
log "v5 --normal verify:"
verify_recovery_byname "$MANI" "$RDIR_VN"

# Test C: v5 --journal - 突破天花板, 期望恢复出文件
RDIR_VJ=$RECOVER_BASE/${TEST}_v5journal
sudo rm -rf "$RDIR_VJ"; mkdir -p "$RDIR_VJ"
log ">>> C) v5 --journal (expected: BREAKTHROUGH - recover from journal) ..."
sudo "$V5" --journal --dir "$RDIR_VJ" "$DEV" 2>&1 | tail -15
log "v5 --journal verify:"
verify_recovery_byname "$MANI" "$RDIR_VJ"

echo "##################### T2 DONE #####################"
