#!/bin/bash
# T0a: 大文件多级extent恢复 baseline (金标准回归门)
# 同一删除后磁盘状态, 先跑原始程序, 再跑v5 --normal, 对比"原能力不丢"
source /mnt/work/testfw/lib_recover_test.sh

ORIG=/home/ubuntu/ext4_recover/src/ext4recover
V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb1
TEST=t0a_largefile
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T0a 大文件 baseline #####################"

# 0. 确保原始程序已编译
if [ ! -x "$ORIG" ]; then
    log "compiling original ext4recover..."
    ( cd /home/ubuntu/ext4_recover/src && make ) || die "compile orig failed"
fi

# 1. 构造目标盘 (限制inode数以加速原程序全inode遍历)
prepare_target "$DEV" "-b4096 -N 262144"

# 2. 写2GB无碎片大文件 (2GB => 16个128MB extent => depth>=1 多级树)
log "writing 2GB bigfile (urandom)..."
dd if=/dev/urandom of=$TARGET_MNT/bigfile bs=1M count=2048 status=none
sync
record_file "$TARGET_MNT/bigfile" "$MANI"
INO=$(stat -c %i $TARGET_MNT/bigfile)
log "bigfile inode=$INO  extent form (filefrag):"
sudo filefrag -v $TARGET_MNT/bigfile 2>/dev/null | head -8
EXTCNT=$(sudo filefrag $TARGET_MNT/bigfile 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
log "extent count = $EXTCNT (需>4才能形成多级树/可被原程序恢复)"

# 3. 落盘 + 删除 + umount
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
log "rm bigfile..."
rm -f $TARGET_MNT/bigfile
detach_target

# 4. 原始程序恢复 (恢复物落 vdc, 不在目标盘上)
RDIR_ORIG=$RECOVER_BASE/${TEST}_orig
sudo rm -rf "$RDIR_ORIG"; mkdir -p "$RDIR_ORIG"
log ">>> running ORIGINAL ext4recover on $DEV ..."
( cd "$RDIR_ORIG" && sudo "$ORIG" "$DEV" 2>&1 | tail -6 )
log "original recovered files:"
sudo ls -la "$RDIR_ORIG/RECOVER" 2>/dev/null | head
echo "----- VERIFY (original) -----"
verify_recovery_baseline "$MANI" "$RDIR_ORIG/RECOVER"
echo "ORIG_RESULT=$?"

# 5. v5 --normal 复跑同一磁盘状态 (验证原能力不丢)
if [ -x "$V5" ]; then
    RDIR_V5=$RECOVER_BASE/${TEST}_v5normal
    sudo rm -rf "$RDIR_V5"; mkdir -p "$RDIR_V5"
    log ">>> running v5 --normal on $DEV ..."
    sudo "$V5" --normal --dir "$RDIR_V5" "$DEV" 2>&1 | tail -6
    echo "----- VERIFY (v5 --normal) -----"
    verify_recovery_baseline "$MANI" "$RDIR_V5"
    echo "V5NORMAL_RESULT=$?"
else
    log "v5 binary not found, skip v5 cross-check"
fi

echo "##################### T0a DONE #####################"
