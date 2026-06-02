#!/bin/bash
# T0b: 碎片小文件多级extent recovery baseline
# 通过强制碎片化制造 200KB 小文件 -> 5+ extent -> depth>=1 -> 可被原程序恢复
source /mnt/work/testfw/lib_recover_test.sh

ORIG=/home/ubuntu/ext4_recover/src/ext4recover
DEV=/dev/vdb2  # 2T小文件盘
TEST=t0b_smallfile_frag
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T0b 碎片小文件 baseline #####################"

prepare_target "$DEV" "-b4096"

# 制造碎片化: 用 fallocate 打洞 + 交错写, 让小文件分布到多个extent
# 策略: 先创建大量 4K 占位文件, 然后删除偶数序列 -> 文件系统空闲块碎片化
log "creating fragmentation pattern..."
mkdir -p $TARGET_MNT/holes
for i in $(seq 1 4000); do
    dd if=/dev/urandom of=$TARGET_MNT/holes/h$i bs=4096 count=1 status=none
done
sync
# 删一半造成碎片
for i in $(seq 2 2 4000); do
    rm $TARGET_MNT/holes/h$i
done
sync
log "fragment done. now writing target small files..."

# 写3个测试小文件 ~200KB, 期望它们落在碎片空闲块上 -> 多 extent
mkdir -p $TARGET_MNT/test
for i in 1 2 3; do
    dd if=/dev/urandom of=$TARGET_MNT/test/sf$i bs=4096 count=50 status=none
    sync
    record_file "$TARGET_MNT/test/sf$i" "$MANI"
    INO=$(stat -c %i $TARGET_MNT/test/sf$i)
    EXT=$(sudo filefrag $TARGET_MNT/test/sf$i 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
    log "  sf$i ino=$INO extents=$EXT"
done

# 删除其余holes释放extent节点
rm -rf $TARGET_MNT/holes
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

# 删目标文件
log "rm target files..."
rm -f $TARGET_MNT/test/sf*
detach_target

# 原始程序恢复
RDIR=$RECOVER_BASE/${TEST}_orig
sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
log ">>> running ORIGINAL ext4recover on $DEV ..."
( cd "$RDIR" && sudo "$ORIG" "$DEV" 2>&1 | tail -10 )
log "recovered list:"
sudo ls -la "$RDIR/RECOVER" 2>/dev/null

# 校验: 用sudo读避开权限问题
log ">>> verifying ..."
total=0; ok=0
while IFS='|' read -r ino size md5 path; do
    total=$((total+1))
    rf="$RDIR/RECOVER/${ino}_file"
    if sudo test ! -f "$rf"; then
        echo "  MISS ino=$ino"
        continue
    fi
    rmd5=$(sudo head -c "$size" "$rf" | md5sum | awk '{print $1}')
    if [ "$rmd5" = "$md5" ]; then
        ok=$((ok+1))
        echo "  OK   ino=$ino size=$size md5=$rmd5"
    else
        echo "  FAIL ino=$ino orig=$md5 got=$rmd5"
    fi
done < "$MANI"
echo "T0B_RESULT: $ok/$total"
