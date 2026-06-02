#!/bin/bash
# ext4recover 实盘测试框架 - 公共库 (v2: 改进verify)
set -u

WORK=/mnt/work
TARGET_MNT=/mnt/target
LOG_DIR=$WORK/logs
MANIFEST_DIR=$WORK/manifests
RECOVER_BASE=$WORK/recover_out

ALLOWED_DEVS="/dev/vdb1 /dev/vdb2 /dev/vdb3 /dev/vdb4 /dev/vdb5 /dev/vdb6 /dev/vdc"

die() { echo "[FATAL] $*" >&2; exit 1; }
log() { echo "[$(date +%H:%M:%S)] $*"; }

assert_allowed_dev() {
    local dev="$1"
    for d in $ALLOWED_DEVS; do
        [ "$dev" = "$d" ] && return 0
    done
    die "DEVICE $dev NOT IN WHITELIST! refuse to operate."
}

prepare_target() {
    local dev="$1"; shift
    local extra="$*"
    assert_allowed_dev "$dev"
    sudo umount "$dev" 2>/dev/null
    sudo umount "$TARGET_MNT" 2>/dev/null
    log "mkfs $dev (opts: $extra)"
    sudo mkfs.ext4 -q -F -m0 $extra "$dev" || die "mkfs failed"
    sudo mount "$dev" "$TARGET_MNT" || die "mount failed"
    sudo chown ubuntu:ubuntu "$TARGET_MNT"
}

record_file() {
    local f="$1" mani="$2"
    local ino size md5
    ino=$(stat -c %i "$f")
    size=$(stat -c %s "$f")
    md5=$(md5sum "$f" | awk '{print $1}')
    echo "$ino|$size|$md5|$f" >> "$mani"
}

check_extent_form() {
    local f="$1"
    sudo filefrag -v "$f" 2>/dev/null | grep -E "ext:|extents found" | tail -3
}

detach_target() {
    sync
    sudo umount "$TARGET_MNT" 2>/dev/null
}

# 改进的verify: 多种恢复文件名查找 + sudo读权限 + 精确截断md5
# manifest格式: ino|size|md5|original_path
# 恢复文件可能命名为: {ino}_file / {basename(path)} / aggressive_<blk> 等
verify_recovery_baseline() {
    local mani="$1" rdir="$2"
    local total=0 ok=0
    while IFS='|' read -r ino size md5 path; do
        total=$((total+1))
        local basename_path
        basename_path=$(basename "$path")
        # 候选恢复文件路径 (多种命名约定)
        local rf=""
        for cand in "$rdir/${ino}_file" "$rdir/$basename_path" "$rdir/${basename_path}.recovered"; do
            if sudo test -f "$cand"; then
                rf="$cand"
                break
            fi
        done
        # fallback: 在rdir里找任何与inode相关的文件
        if [ -z "$rf" ]; then
            rf=$(sudo find "$rdir" -maxdepth 1 -type f \( -name "${ino}_file" -o -name "$basename_path" \) 2>/dev/null | head -1)
        fi
        if [ -z "$rf" ] || ! sudo test -f "$rf"; then
            echo "  MISS ino=$ino size=$size $basename_path"
            continue
        fi
        # 用 sudo 读取并截断到原 size 再 md5
        local rmd5
        rmd5=$(sudo head -c "$size" "$rf" 2>/dev/null | md5sum | awk '{print $1}')
        if [ "$rmd5" = "$md5" ]; then
            ok=$((ok+1))
            echo "  OK   ino=$ino size=$size $basename_path  -> $(basename "$rf")"
        else
            echo "  FAIL ino=$ino md5 mismatch (orig=$md5 got=$rmd5) $basename_path -> $(basename "$rf")"
        fi
    done < "$mani"
    echo "RESULT: $ok/$total recovered (md5 match after size-trunc)"
    [ "$ok" -gt 0 ] && [ "$ok" -eq "$total" ]
}

log "lib loaded (v2). WORK=$WORK"
