#!/bin/bash
# T1: v5 全5模式实盘验证 (大文件 2GB, 复用T0a场景)
# 在同一磁盘状态下,依次跑 normal/journal/orphan/aggressive/all,验证md5一致性
source /mnt/work/testfw/lib_recover_test.sh

verify_recovery_baseline() {
    local mani="$1" rdir="$2"
    local total=0 ok=0
    while IFS='|' read -r ino size md5 path; do
        total=$((total+1))
        local rf=$(sudo find "$rdir" -name "${ino}_file" 2>/dev/null | head -1)
        [ -z "$rf" ] && rf="$rdir/${ino}_file"
        if sudo test ! -f "$rf"; then
            echo "  MISS ino=$ino"; continue
        fi
        local rmd5=$(sudo head -c "$size" "$rf" 2>/dev/null | md5sum | awk '{print $1}')
        if [ "$rmd5" = "$md5" ]; then
            ok=$((ok+1)); echo "  OK   ino=$ino md5=$rmd5"
        else
            echo "  FAIL ino=$ino orig=$md5 got=$rmd5"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total"
    [ "$ok" -gt 0 ] && [ "$ok" -eq "$total" ]
}

V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb1
TEST=t1_v5_modes
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T1 v5 全模式回归 (2GB 大文件) #####################"

# 准备目标盘
prepare_target "$DEV" "-b4096 -N 262144"
log "writing 2GB bigfile..."
dd if=/dev/urandom of=$TARGET_MNT/bigfile bs=1M count=2048 status=none
sync
record_file "$TARGET_MNT/bigfile" "$MANI"
INO=$(stat -c %i $TARGET_MNT/bigfile)
EXTC=$(sudo filefrag $TARGET_MNT/bigfile 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
log "bigfile inode=$INO extents=$EXTC"

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
log "rm bigfile..."
rm -f $TARGET_MNT/bigfile
detach_target

# 由于不同模式会改变inode/journal状态(尤其是journal/orphan),
# 为了公平比较,需要在每次模式之间重新构造同样的删除态
# 但每次都重建2G文件太慢, 先跑5个模式都用同一个删除态(大部分模式都只读)

# 注意: orig的恢复物可能已经存在, 可保留作对照
declare -A RESULTS
for MODE in normal journal orphan aggressive all; do
    RDIR=$RECOVER_BASE/${TEST}_${MODE}
    sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
    log ">>> v5 --$MODE on $DEV ..."
    sudo "$V5" --$MODE --dir "$RDIR" "$DEV" > /tmp/v5_$MODE.out 2>&1
    log "stats:"
    grep -E 'Total files|Journal recov|Orphan recov|Aggressive recov|Files failed' /tmp/v5_$MODE.out | head -5
    log "verify --$MODE:"
    if verify_recovery_baseline "$MANI" "$RDIR"; then
        RESULTS[$MODE]=PASS
    else
        RESULTS[$MODE]=FAIL
    fi
    echo
done

echo "================ T1 SUMMARY ================"
for MODE in normal journal orphan aggressive all; do
    printf "  %-12s : %s\n" "--$MODE" "${RESULTS[$MODE]}"
done
echo "============================================="
