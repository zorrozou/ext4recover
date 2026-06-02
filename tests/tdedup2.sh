#!/bin/bash
# T-DEDUP-2: 验证 aggressive 阶段下 dedup 跨 phase 跳过工作
# 在小分区 vdb5 (500G) 上跑 --all，让 journal phase 先 dump 文件，
# 然后 aggressive phase 全盘扫，期望大量 leaf 块被 dedup 跳过。
set -u
. /mnt/work/testfw/lib_recover_test.sh

V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb5
TEST=tdedup2
MANI=$MANIFEST_DIR/$TEST.mani
RDIR=$RECOVER_BASE/$TEST
sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
rm -f "$MANI"

echo "##################### T-DEDUP-2: --all dedup with aggressive #####################"
prepare_target $DEV "-b4096"
log "step1: write 100MB random file"
sudo dd if=/dev/urandom of=$TARGET_MNT/big bs=1M count=100 status=none
sync
echo "filefrag:"
sudo filefrag -v $TARGET_MNT/big | head -5
record_file "$TARGET_MNT/big" "$MANI"
log "step2: rm + sync"
sudo rm $TARGET_MNT/big
sync
detach_target

# --all 启用 normal+orphan+journal+aggressive
log ">>> v5 --all (will scan whole 500G partition, ~25min for aggressive)"
log "    starting in background, will check periodically"
sudo nohup $V5 --all --dir "$RDIR" $DEV > /mnt/work/logs/tdedup2_v5.log 2>&1 &
V5PID=$!
disown
log "v5 PID=$V5PID, waiting..."

# 等 v5 完成或最多 30 分钟
for i in $(seq 1 36); do
    sleep 50
    if ! sudo ps -p $V5PID > /dev/null 2>&1; then
        log "v5 finished at iteration $i (~$((i*50))s)"
        break
    fi
    log "  iter $i: still running, $(sudo ps -p $V5PID -o etime= 2>/dev/null)"
done

# 如果还在跑就杀
if sudo ps -p $V5PID > /dev/null 2>&1; then
    log "v5 still running after 30min, killing"
    sudo kill -9 $V5PID 2>/dev/null
fi

echo
log "=== final stats ==="
grep -E 'Dedup|Total files|Journal recovered|Aggressive recovered|leaf .* fully covered|fully covered' \
    /mnt/work/logs/tdedup2_v5.log | head -30

echo
log "=== products ==="
sudo ls -la "$RDIR" | head -20

echo
log "=== verify ==="
verify_recovery_baseline "$MANI" "$RDIR"
