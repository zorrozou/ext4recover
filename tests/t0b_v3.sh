#!/bin/bash
# T0b v3: 用满分区策略强制小文件多级extent
# 在300G小分区上,先用大量4K文件填到95%,再隔行删除制造离散4K空闲块,
# 此后新写的小文件因找不到连续段必然碎片化(多extent->多级树)
source /mnt/work/testfw/lib_recover_test.sh

# 用audit.sh里的修复版verify
verify_recovery_baseline() {
    local mani="$1" rdir="$2"
    local total=0 ok=0
    while IFS='|' read -r ino size md5 path; do
        total=$((total+1))
        local rf=$(sudo find "$rdir" -name "${ino}_file" 2>/dev/null | head -1)
        [ -z "$rf" ] && rf="$rdir/${ino}_file"
        if sudo test ! -f "$rf"; then
            echo "  MISS ino=$ino size=$size $(basename $path)"; continue
        fi
        local rmd5=$(sudo head -c "$size" "$rf" 2>/dev/null | md5sum | awk '{print $1}')
        if [ "$rmd5" = "$md5" ]; then
            ok=$((ok+1)); echo "  OK   ino=$ino md5=$rmd5"
        else
            echo "  FAIL ino=$ino orig=$md5 got=$rmd5"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total"
}

ORIG=/home/ubuntu/ext4_recover/src/ext4recover
V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb6  # 300G 小分区
TEST=t0b_smallfile_v3
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T0b v3 满分区强制碎片化小文件 #####################"

# 用2GB小分区做这次测试更快(从300G切个loop类似的小段不行,直接reformat小)
# 实际用vdb6 300G,但只填2GB够碎片化了
prepare_target "$DEV" "-b4096"

# Step 1: 在target盘填2GB大量4K占位文件 -> 制造海量inode + 连续块
log "step1: filling with 50000 small files (4K each = 200MB)"
mkdir -p $TARGET_MNT/filler
seq 1 50000 | xargs -n1 -P8 -I{} sh -c "dd if=/dev/urandom of=$TARGET_MNT/filler/f{} bs=4096 count=1 status=none 2>/dev/null"
sync
log "  filler done, count=$(ls $TARGET_MNT/filler | wc -l)"

# Step 2: 删除偶数文件 -> 25000个4K洞分散在25000个4K占位间
log "step2: removing every-other file to fragment free space"
for i in $(seq 2 2 50000); do rm -f $TARGET_MNT/filler/f$i; done
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
log "  remaining filler=$(ls $TARGET_MNT/filler | wc -l)"

# Step 3: 写小文件,期望落进碎片洞
log "step3: writing small target files"
mkdir -p $TARGET_MNT/test
for i in 1 2 3 4 5; do
    dd if=/dev/urandom of=$TARGET_MNT/test/sf$i bs=4096 count=50 status=none
    sync
    INO=$(stat -c %i $TARGET_MNT/test/sf$i)
    EXT=$(sudo filefrag $TARGET_MNT/test/sf$i 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
    log "  sf$i ino=$INO extents=$EXT"
    if [ "$EXT" -ge 5 ]; then
        record_file "$TARGET_MNT/test/sf$i" "$MANI"
    fi
done

# 也制造一个稍大点的 (1MB) 用相同策略, 1MB / 4K = 256个block, 必然碎片化
log "step3b: writing 1MB files for stronger fragmentation"
for i in 6 7 8; do
    dd if=/dev/urandom of=$TARGET_MNT/test/sf$i bs=4096 count=256 status=none
    sync
    INO=$(stat -c %i $TARGET_MNT/test/sf$i)
    EXT=$(sudo filefrag $TARGET_MNT/test/sf$i 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
    log "  sf$i (1MB) ino=$INO extents=$EXT"
    if [ "$EXT" -ge 5 ]; then
        record_file "$TARGET_MNT/test/sf$i" "$MANI"
    fi
done

# Step 4: 释放filler腾出空间(让恢复扫描时不被占用)
log "step4: cleanup filler"
rm -rf $TARGET_MNT/filler
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

# Step 5: 删目标
log "step5: rm target"
rm -f $TARGET_MNT/test/sf*
detach_target

TOTAL=$(wc -l < "$MANI" 2>/dev/null)
log "manifest has $TOTAL multi-extent small files (need >=1 to be valid baseline)"
[ "$TOTAL" -lt 1 ] && { log "FRAGMENTATION_FAIL"; exit 1; }

# Step 6: orig 程序恢复
RDIR_ORIG=$RECOVER_BASE/${TEST}_orig
sudo rm -rf "$RDIR_ORIG"; mkdir -p "$RDIR_ORIG"
log ">>> ORIG ext4recover on $DEV ..."
( cd "$RDIR_ORIG" && sudo "$ORIG" "$DEV" 2>&1 | grep -vE 'Unknown code' | tail -8 )
log "ORIG verify:"
verify_recovery_baseline "$MANI" "$RDIR_ORIG/RECOVER"

# Step 7: v5 --normal
RDIR_V5=$RECOVER_BASE/${TEST}_v5normal
sudo rm -rf "$RDIR_V5"; mkdir -p "$RDIR_V5"
log ">>> v5 --normal on $DEV ..."
sudo "$V5" --normal --dir "$RDIR_V5" "$DEV" 2>&1 | tail -8
log "V5 --normal verify:"
verify_recovery_baseline "$MANI" "$RDIR_V5"

# Step 8: v5 --all
RDIR_V5A=$RECOVER_BASE/${TEST}_v5all
sudo rm -rf "$RDIR_V5A"; mkdir -p "$RDIR_V5A"
log ">>> v5 --all on $DEV ..."
sudo "$V5" --all --dir "$RDIR_V5A" "$DEV" 2>&1 | tail -8
log "V5 --all verify:"
verify_recovery_baseline "$MANI" "$RDIR_V5A"

echo "##################### T0b v3 DONE #####################"
