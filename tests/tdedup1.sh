#!/bin/bash
# T-DEDUP-1: 验证 --all 模式下 dedup 跨 phase 工作
# 同一磁盘状态，跑 --normal --journal （注：v5 没有 --normal --journal 组合，
# 但 --all 会同时启用 normal+journal+orphan+aggressive，足够触发重叠）。
#
# 期望：journal phase 先 dump 12_file/bigfile，normal phase 看到同 inode
# 的 extent 时全部 dedup-skip，最终产物只有一个文件。
set -u
. /mnt/work/testfw/lib_recover_test.sh

V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb1
TEST=tdedup1
MANI=$MANIFEST_DIR/$TEST.mani
RDIR=$RECOVER_BASE/$TEST
sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
rm -f "$MANI"

echo "##################### T-DEDUP-1: --all dedup #####################"
prepare_target $DEV "-b4096"
log "step1: write 600MB random file"
sudo dd if=/dev/urandom of=$TARGET_MNT/big bs=1M count=600 status=none
sync
echo "filefrag:"
sudo filefrag -v $TARGET_MNT/big | head -8
record_file "$TARGET_MNT/big" "$MANI"
log "step2: rm + sync"
sudo rm $TARGET_MNT/big
sync
detach_target

# 注意: --all 不会执行 aggressive (那要 30+min). 我们只需要 normal+journal+orphan
# v5 模式标志位: NORMAL=0x01 ORPHAN=0x02 JOURNAL=0x04 AGGRESSIVE=0x08
# 用 --normal --journal 组合 = 0x05
log ">>> v5 --normal --journal (combined dedup test)"
sudo $V5 --normal --journal --verbose --dir "$RDIR" $DEV 2>&1 \
    | grep -E 'Total files|Files failed|Journal recovered|Dedup|Recovery complete|dedup-skip' \
    | head -20

echo
log "products in $RDIR:"
sudo ls -la "$RDIR"

echo
log "verify md5"
verify_recovery_baseline "$MANI" "$RDIR"
