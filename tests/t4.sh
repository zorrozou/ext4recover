#!/bin/bash
# T4: fallocate预分配 unwritten extent 处理
# fallocate创建的extent ee_len高位置1表示未初始化, 真实长度=ee_len-32768
# 测试v5在删除fallocate文件时是否能正确处理(不溢出/不死循环)
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
            local rsize=$(sudo stat -c %s "$rf" 2>/dev/null)
            echo "  FAIL ino=$ino orig=$md5 got=$rmd5 (orig_size=$size got_size=$rsize)"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total"
}

ORIG=/home/ubuntu/ext4_recover/src/ext4recover
V5=/home/ubuntu/ext4_recover_improved/ext4recover_v5
DEV=/dev/vdb6
TEST=t4_unwritten
MANI=$MANIFEST_DIR/$TEST.mani
rm -f "$MANI"

echo "##################### T4 unwritten extent (fallocate) #####################"

prepare_target "$DEV" "-b4096"

# 准备3个测试文件:
# (1) f_pre: fallocate 1GB 不写任何数据 -> 全是unwritten extent
# (2) f_part: fallocate 1GB 但只写前256MB -> 头部initialized + 尾部unwritten
# (3) f_full: fallocate 1GB 后全部写满 -> 全部initialized (作为对照)
log "step1: create test files"
fallocate -l 1G $TARGET_MNT/f_pre
sync
INO_PRE=$(stat -c %i $TARGET_MNT/f_pre)
log "  f_pre (fallocate 1G,no write) ino=$INO_PRE"
sudo filefrag -v $TARGET_MNT/f_pre 2>/dev/null | head -8

fallocate -l 1G $TARGET_MNT/f_part
dd if=/dev/urandom of=$TARGET_MNT/f_part bs=1M count=256 conv=notrunc status=none
sync
INO_PART=$(stat -c %i $TARGET_MNT/f_part)
log "  f_part (fallocate 1G, write first 256M) ino=$INO_PART"
sudo filefrag -v $TARGET_MNT/f_part 2>/dev/null | head -8
record_file "$TARGET_MNT/f_part" "$MANI"

fallocate -l 1G $TARGET_MNT/f_full
dd if=/dev/urandom of=$TARGET_MNT/f_full bs=1M count=1024 conv=notrunc status=none
sync
INO_FULL=$(stat -c %i $TARGET_MNT/f_full)
log "  f_full (fallocate 1G, fully written) ino=$INO_FULL"
sudo filefrag -v $TARGET_MNT/f_full 2>/dev/null | head -8
record_file "$TARGET_MNT/f_full" "$MANI"

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

log "step2: rm files"
rm -f $TARGET_MNT/f_pre $TARGET_MNT/f_part $TARGET_MNT/f_full
detach_target

# 测试: 重点看v5是否崩溃/产生异常
RDIR=$RECOVER_BASE/${TEST}_orig
sudo rm -rf "$RDIR"; mkdir -p "$RDIR"
log ">>> ORIG ext4recover (test crash safety) ..."
( cd "$RDIR" && timeout 60 sudo "$ORIG" "$DEV" 2>&1 | grep -vE 'Unknown code' | tail -10 )
log "ORIG verify:"
verify_recovery_byname "$MANI" "$RDIR/RECOVER"

RDIR_VN=$RECOVER_BASE/${TEST}_v5normal
sudo rm -rf "$RDIR_VN"; mkdir -p "$RDIR_VN"
log ">>> v5 --normal ..."
timeout 60 sudo "$V5" --normal --dir "$RDIR_VN" "$DEV" 2>&1 | tail -10
log "v5 normal verify:"
verify_recovery_byname "$MANI" "$RDIR_VN"

RDIR_VJ=$RECOVER_BASE/${TEST}_v5journal
sudo rm -rf "$RDIR_VJ"; mkdir -p "$RDIR_VJ"
log ">>> v5 --journal ..."
timeout 60 sudo "$V5" --journal --dir "$RDIR_VJ" "$DEV" 2>&1 | tail -10
log "v5 journal verify:"
verify_recovery_byname "$MANI" "$RDIR_VJ"

echo "##################### T4 DONE #####################"
