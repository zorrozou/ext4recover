#!/bin/bash
# T0b v2: 强制构造碎片小文件
# 策略: 用 fallocate punch-hole 把大文件打成 swiss cheese,
# 释放出离散4K洞,再让小文件落进这些洞里,自然碎片化
source /mnt/work/testfw/lib_recover_test.sh

ORIG=/home/ubuntu/ext4_recover/src/ext4recover
DEV=/dev/vdb2
TEST=t0b_smallfile_frag
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T0b v2 强制碎片小文件 #####################"

prepare_target "$DEV" "-b4096"

# 方法: 创建一个 ~400MB 的占位文件填满分区前段,
# 用 punch-hole 在 4K 偏移处每隔 32K 打 4K 洞 => 12500 个4K离散空闲块,
# 然后写小文件到这些洞中,mballoc 会因找不到大块连续空间而碎片化分配
log "step1: create 400MB filler"
dd if=/dev/urandom of=$TARGET_MNT/filler bs=1M count=400 status=none
sync

log "step2: punch holes every 32K -> 12500 discrete 4K free blocks"
# 用一个python脚本批量打洞,避免shell循环慢
sudo python3 -c "
import os, fcntl, ctypes
FALLOC_FL_PUNCH_HOLE=0x02
FALLOC_FL_KEEP_SIZE=0x01
libc=ctypes.CDLL('libc.so.6')
fd=os.open('$TARGET_MNT/filler', os.O_RDWR)
n=0
for off in range(4096, 400*1024*1024, 32*1024):
    libc.fallocate(fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE, ctypes.c_int64(off), ctypes.c_int64(4096))
    n+=1
print('punched',n,'holes')
os.close(fd)
" || die "punch hole failed"
sync

log "step3: write small files (200KB each, expecting fragmented allocation)"
mkdir -p $TARGET_MNT/test
for i in 1 2 3 4 5; do
    dd if=/dev/urandom of=$TARGET_MNT/test/sf$i bs=4096 count=50 status=none
    sync
    INO=$(stat -c %i $TARGET_MNT/test/sf$i)
    EXT=$(sudo filefrag $TARGET_MNT/test/sf$i 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
    DEPTH=$(sudo debugfs -R "stat <$INO>" /dev/vdb2 2>/dev/null | grep -oE 'tree depth [0-9]+' | head -1)
    log "  sf$i ino=$INO extents=$EXT $DEPTH"
    # 只把多extent的小文件加入清单(才会被原程序恢复)
    if [ "$EXT" -ge 4 ]; then
        record_file "$TARGET_MNT/test/sf$i" "$MANI"
        echo "    -> recorded (multi-extent)"
    fi
done

# 删除filler让其释放更多块,便于aggressive扫描
log "step4: rm filler + sync"
rm -f $TARGET_MNT/filler
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

log "step5: rm target small files"
rm -f $TARGET_MNT/test/sf*
detach_target

# 检查清单
TOTAL=$(wc -l < "$MANI")
log "manifest has $TOTAL multi-extent small files"
if [ "$TOTAL" -eq 0 ]; then
    log "WARN: no multi-extent small files generated, fragmentation strategy failed"
    log "try increase punch-hole density or use bigger filler"
fi

# 原始程序恢复
RDIR=$RECOVER_BASE/${TEST}_orig
sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
log ">>> running ORIGINAL ext4recover on $DEV ..."
( cd "$RDIR" && sudo "$ORIG" "$DEV" 2>&1 | tail -15 )
log "recovered files (multi-ext only):"
sudo ls -la "$RDIR/RECOVER" 2>/dev/null

# 校验
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
        ok=$((ok+1)); echo "  OK   ino=$ino size=$size"
    else
        echo "  FAIL ino=$ino orig=$md5 got=$rmd5"
    fi
done < "$MANI"
echo "T0B_V2_RESULT: $ok/$total (multi-extent small files md5-matched)"
