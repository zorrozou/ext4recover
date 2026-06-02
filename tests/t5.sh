#!/bin/bash
# T5: journal wrap 边界量化
# 目标: 删除小文件后,逐步增加新写入量,测试v5 --journal何时失效
# 输出: 写多少MB能让旧inode副本被覆盖
source /mnt/work/testfw/lib_recover_test.sh

verify_recovery_byname() {
    local mani="$1" rdir="$2"
    local total=0 ok=0
    while IFS='|' read -r ino size md5 path; do
        total=$((total+1))
        local fname=$(basename "$path")
        local rf=$(sudo find "$rdir" -name "$fname" -o -name "${ino}_file" 2>/dev/null | head -1)
        if [ -z "$rf" ] || sudo test ! -f "$rf"; then continue; fi
        local rmd5=$(sudo head -c "$size" "$rf" 2>/dev/null | md5sum | awk '{print $1}')
        [ "$rmd5" = "$md5" ] && ok=$((ok+1))
    done < "$mani"
    echo "$ok/$total"
}

V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb4   # 用vdb4 (500G,可指定小journal)
TEST=t5_journal_wrap

echo "##################### T5 journal wrap 边界量化 #####################"

# 用小journal: -J size=64MB,这样很快wrap
prepare_target "$DEV" "-b4096 -J size=64"
log "journal size: 64MB"

# 创建10个测试小文件
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"
mkdir -p $TARGET_MNT/sf
for i in 1 2 3 4 5 6 7 8 9 10; do
    dd if=/dev/urandom of=$TARGET_MNT/sf/f$i bs=1024 count=200 status=none
    record_file "$TARGET_MNT/sf/f$i" "$MANI"
done
sync
log "created 10 small files (200KB each)"

log "step1: rm files"
rm -f $TARGET_MNT/sf/*
sync

# 在分区还挂载状态下,写新数据观察journal wrap影响
# 每写一定量数据后跑一次 v5 --journal,记录恢复成功率
for WRITE_MB in 0 16 32 64 128 256 512 1024; do
    if [ "$WRITE_MB" -gt 0 ]; then
        # 写新数据触发journal活动
        dd if=/dev/urandom of=$TARGET_MNT/noise.bin bs=1M count=$WRITE_MB conv=notrunc status=none oflag=sync
        sync
        rm -f $TARGET_MNT/noise.bin
        sync
    fi
    
    # 卸载后跑v5 --journal
    sudo umount $TARGET_MNT 2>/dev/null
    RDIR=$RECOVER_BASE/${TEST}_w${WRITE_MB}
    sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
    timeout 60 sudo "$V5" --journal --dir "$RDIR" "$DEV" >/tmp/t5_w${WRITE_MB}.out 2>&1
    RES=$(verify_recovery_byname "$MANI" "$RDIR")
    JREC=$(grep -oE 'Journal recovered: [0-9]+' /tmp/t5_w${WRITE_MB}.out | head -1)
    log "after writing ${WRITE_MB}MB noise: recovered=${RES}  ($JREC)"
    
    # 重新挂载继续测试
    if [ "$WRITE_MB" != "$(echo 1024)" ]; then
        sudo mount $DEV $TARGET_MNT
    fi
done

sudo umount $TARGET_MNT 2>/dev/null
echo "##################### T5 DONE #####################"
