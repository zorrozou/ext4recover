#!/bin/bash
# T6: bigalloc cluster_size=64K 实盘验证
# v5 README声称支持bigalloc, 实盘验证md5一致性
source /mnt/work/testfw/lib_recover_test.sh

verify_recovery_byname() {
    local mani="$1" rdir="$2"
    local total=0 ok=0
    while IFS='|' read -r ino size md5 path; do
        total=$((total+1))
        local fname=$(basename "$path")
        local rf=$(sudo find "$rdir" -name "$fname" -o -name "${ino}_file" 2>/dev/null | head -1)
        if [ -z "$rf" ] || sudo test ! -f "$rf"; then
            echo "  MISS ino=$ino name=$fname"; continue
        fi
        local rmd5=$(sudo head -c "$size" "$rf" 2>/dev/null | md5sum | awk '{print $1}')
        if [ "$rmd5" = "$md5" ]; then
            ok=$((ok+1)); echo "  OK   ino=$ino name=$fname md5=$rmd5"
        else
            echo "  FAIL ino=$ino orig=$md5 got=$rmd5"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total"
}

V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb5   # 500G,专用bigalloc
TEST=t6_bigalloc

echo "##################### T6 bigalloc cluster=64K #####################"

# bigalloc + cluster=64KB
sudo umount $DEV 2>/dev/null
log "mkfs $DEV with bigalloc cluster_size=64K"
sudo mkfs.ext4 -q -F -m0 -b 4096 -O bigalloc -C 65536 $DEV || die "mkfs bigalloc failed"
sudo mount $DEV /mnt/target
sudo chown ubuntu:ubuntu /mnt/target

# 验证bigalloc启用
log "fs features:"
sudo dumpe2fs -h $DEV 2>/dev/null | grep -E 'Filesystem features|Block size|Cluster size'

MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

# 写测试文件:
# (1) bigfile 100MB大文件 (跨多cluster)
# (2) 5个200KB小文件
log "step1: write 100MB big file"
dd if=/dev/urandom of=/mnt/target/bigfile bs=1M count=100 status=none
sync
record_file "/mnt/target/bigfile" "$MANI"
INO=$(stat -c %i /mnt/target/bigfile)
EXT=$(sudo filefrag /mnt/target/bigfile 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
log "  bigfile ino=$INO extents=$EXT"

log "step2: write 5 small files 200KB each"
mkdir -p /mnt/target/sf
for i in 1 2 3 4 5; do
    dd if=/dev/urandom of=/mnt/target/sf/f$i bs=1024 count=200 status=none
    sync
    record_file "/mnt/target/sf/f$i" "$MANI"
done

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
log "step3: rm all"
rm -f /mnt/target/bigfile /mnt/target/sf/*
sudo umount /mnt/target

# Test v5 三种模式
for MODE in normal journal all; do
    RDIR=$RECOVER_BASE/${TEST}_v5${MODE}
    sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
    log ">>> v5 --$MODE on bigalloc fs ..."
    timeout 120 sudo "$V5" --$MODE --dir "$RDIR" "$DEV" 2>&1 | grep -E 'bigalloc|cluster|Total files|recov|failed' | head -10
    log "verify --$MODE:"
    verify_recovery_byname "$MANI" "$RDIR"
    echo
done

echo "##################### T6 DONE #####################"
