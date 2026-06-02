#!/bin/bash
# T3: 碎片化小文件 - 通过禁用mballoc预分配 + 极满分区强制ext4碎片化
# 目标: 让小文件形成 >=5 extent (depth>=1, 多级树),
#       使其在删除后仍可被原程序+v5--normal恢复
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
            ok=$((ok+1)); echo "  OK   ino=$ino name=$fname"
        else
            echo "  FAIL ino=$ino orig=$md5 got=$rmd5"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total"
}

ORIG=/home/ubuntu/ext4_recover/src/ext4recover
V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb6
TEST=t3_smallfile_frag
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T3 碎片化小文件强制构造 #####################"

prepare_target "$DEV" "-b4096"

# 关键: 调整mballoc参数让ext4不预分配大段连续空间
DEVNAME=$(basename $DEV)
log "step0: tune mballoc - mb_group_prealloc=1, stream_req=0"
echo 1 | sudo tee /sys/fs/ext4/$DEVNAME/mb_group_prealloc >/dev/null 2>&1
echo 0 | sudo tee /sys/fs/ext4/$DEVNAME/mb_stream_req >/dev/null 2>&1
echo 1 | sudo tee /sys/fs/ext4/$DEVNAME/mb_min_to_scan >/dev/null 2>&1
sudo cat /sys/fs/ext4/$DEVNAME/mb_group_prealloc 2>/dev/null

# step1: 写一个大占位文件填到极满 (留 ~50MB 余量),
# 然后用python批量打洞,每隔16K打8K洞 => 极度散乱的8K自由块
log "step1: fill device near full (leaving ~50MB)"
AVAIL=$(df -B1 $TARGET_MNT | tail -1 | awk '{print $4}')
RESERVE=$((50 * 1024 * 1024))  # 50MB余量
FILL=$(( (AVAIL - RESERVE) / 1024 / 1024 ))  # MB
log "available=${AVAIL}B fill=${FILL}MB"
dd if=/dev/zero of=$TARGET_MNT/big bs=1M count=$FILL status=none
sync
log "  big file size=$(stat -c %s $TARGET_MNT/big 2>/dev/null)"

log "step2: punch dense holes in big (every 16K punch 8K -> very fragmented free space)"
sudo python3 -c "
import os, ctypes
FALLOC_FL_PUNCH_HOLE=0x02
FALLOC_FL_KEEP_SIZE=0x01
libc=ctypes.CDLL('libc.so.6')
fd=os.open('$TARGET_MNT/big', os.O_RDWR)
sz=os.fstat(fd).st_size
n=0
# 每隔16K打8K洞,在前2GB密集打
limit=min(sz, 2*1024*1024*1024)
for off in range(8192, limit, 16384):
    r=libc.fallocate(fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
                     ctypes.c_int64(off), ctypes.c_int64(8192))
    if r==0: n+=1
print('punched',n,'holes (8K each)')
os.close(fd)
"
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

log "step3: write 5 small files (200KB each), should be FORCED fragmented"
mkdir -p $TARGET_MNT/sf
for i in 1 2 3 4 5; do
    dd if=/dev/urandom of=$TARGET_MNT/sf/sf$i bs=4096 count=50 status=none
    sync
    INO=$(stat -c %i $TARGET_MNT/sf/sf$i)
    EXT=$(sudo filefrag $TARGET_MNT/sf/sf$i 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
    log "  sf$i ino=$INO extents=$EXT"
    if [ "$EXT" -ge 5 ]; then
        record_file "$TARGET_MNT/sf/sf$i" "$MANI"
    fi
done

# step4: 再写3个500KB文件 (>4个extent就是多级)
log "step4: write 3 medium files (500KB each)"
for i in 1 2 3; do
    dd if=/dev/urandom of=$TARGET_MNT/sf/mf$i bs=4096 count=125 status=none
    sync
    INO=$(stat -c %i $TARGET_MNT/sf/mf$i)
    EXT=$(sudo filefrag $TARGET_MNT/sf/mf$i 2>/dev/null | grep -oE '[0-9]+ extent' | grep -oE '[0-9]+')
    log "  mf$i ino=$INO size=500K extents=$EXT"
    if [ "$EXT" -ge 5 ]; then
        record_file "$TARGET_MNT/sf/mf$i" "$MANI"
    fi
done

# 释放占位文件 (这步会让后续aggressive扫到的extent header大量浮现)
log "step5: rm big filler"
rm -f $TARGET_MNT/big
sync

# 删目标
log "step6: rm test small files"
rm -f $TARGET_MNT/sf/*
detach_target

TOTAL=$(wc -l < "$MANI" 2>/dev/null)
log "manifest has $TOTAL multi-extent small files"
[ "$TOTAL" -lt 1 ] && { log "FRAGMENTATION_FAIL"; exit 1; }

# orig
RDIR=$RECOVER_BASE/${TEST}_orig
sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
log ">>> ORIG ext4recover ..."
( cd "$RDIR" && sudo "$ORIG" "$DEV" 2>&1 | grep -vE 'Unknown code' | tail -8 )
log "ORIG verify:"
verify_recovery_byname "$MANI" "$RDIR/RECOVER"

# v5 normal
RDIR_VN=$RECOVER_BASE/${TEST}_v5normal
sudo rm -rf "$RDIR_VN"; mkdir -p "$RDIR_VN"
log ">>> v5 --normal ..."
sudo "$V5" --normal --dir "$RDIR_VN" "$DEV" 2>&1 | grep -E 'Total files|recovered:|failed' | head -5
log "v5 normal verify:"
verify_recovery_byname "$MANI" "$RDIR_VN"

# v5 journal
RDIR_VJ=$RECOVER_BASE/${TEST}_v5journal
sudo rm -rf "$RDIR_VJ"; mkdir -p "$RDIR_VJ"
log ">>> v5 --journal ..."
sudo "$V5" --journal --dir "$RDIR_VJ" "$DEV" 2>&1 | grep -E 'Total files|recovered:|failed' | head -5
log "v5 journal verify:"
verify_recovery_byname "$MANI" "$RDIR_VJ"

echo "##################### T3 DONE #####################"
