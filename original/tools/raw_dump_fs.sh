#!/bin/bash

# 检查参数
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <device> [superblock_block_num] [block_size]"
    echo "Example 1 (Primary SB): $0 /dev/vda1"
    echo "Example 2 (Backup SB):  $0 /dev/vda1 32768"
    echo "Example 3 (Custom BS):  $0 /dev/vda1 98304 4096"
    exit 1
fi

DEVICE=$1
SB_BLOCK_NUM=${2:-0}     # 默认为 0 (代表主超级块逻辑)
BLOCK_SIZE_ARG=${3:-4096} # 默认块大小为 4k，用于计算偏移量

# 检查设备是否存在
if [ ! -b "$DEVICE" ] && [ ! -f "$DEVICE" ]; then
    echo "Error: Device $DEVICE not found or not a block device."
    exit 1
fi

# 计算字节偏移量
if [ "$SB_BLOCK_NUM" -eq 0 ]; then
    # 主 Superblock 总是位于 1024 字节偏移处
    BYTE_OFFSET=1024
    echo "Target: Primary Superblock (Offset 1024 bytes)"
else
    # 备份 Superblock 位于 Block_Number * Block_Size
    # 使用 bc 处理可能的大数乘法
    BYTE_OFFSET=$(echo "$SB_BLOCK_NUM * $BLOCK_SIZE_ARG" | bc)
    echo "Target: Backup Superblock at block $SB_BLOCK_NUM (Assumed Block Size: $BLOCK_SIZE_ARG)"
    echo "        Calculated Byte Offset: $BYTE_OFFSET"
fi

# 临时变量存储 Hex 字符串
# 读取 Superblock (Length 1024 bytes)
# 使用 hexdump 将其转换为连续的 hex 字符串
RAW_HEX=$(hexdump -v -s "$BYTE_OFFSET" -n 1024 -e '1/1 "%02x"' "$DEVICE" 2>/dev/null)

if [ -z "$RAW_HEX" ]; then
    echo "Error: Failed to read from device. Check permissions (need root?) or device path."
    exit 1
fi

# --- 辅助函数：从 HEX 串中提取数值 (Little Endian) ---

# $1: Byte Offset (Decimal), $2: Byte Length (1, 2, 4)
get_val() {
    local offset=$1
    local len=$2
    local hex_subset
    local val=0
    
    # Bash 字符串索引是从 0 开始，每个字节占 2 个 hex 字符
    local start=$((offset * 2))
    local length=$((len * 2))
    
    hex_subset=${RAW_HEX:$start:$length}
    
    # Little Endian 转换: 
    for (( i=0; i<len; i++ )); do
        local byte_hex=${hex_subset:$((i*2)):2}
        local byte_val=$((16#$byte_hex))
        val=$(echo "$val + $byte_val * (256 ^ $i)" | bc)
    done
    
    echo "$val"
}

# 提取字符串 (C-string style: 遇到第一个空字符停止)
# 返回的字符串可能包含首尾空白符/换行符，需要在调用处使用 xargs 清理
get_string() {
    local offset=$1
    local len=$2
    local start=$((offset * 2))
    local length=$((len * 2))
    local hex_subset=${RAW_HEX:$start:$length}
    
    # 1. 转换为 ASCII 字节
    # 2. 使用 awk 截断到第一个空字符 (C-string 终止符)
    #    -F'\0' 将空字符设置为空字符，只输出第一个字段 ($1)
    echo "$hex_subset" | xxd -r -p | awk -F'\0' '{print $1}'
}

# 提取 UUID
get_uuid() {
    local offset=104 # 0x68
    local hex=${RAW_HEX:$((offset*2)):32}
    # Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    echo "${hex:0:8}-${hex:8:4}-${hex:12:4}-${hex:16:4}-${hex:20:12}"
}

# 格式化时间戳
format_time() {
    local ts=$1
    if [ "$ts" -eq 0 ]; then
        echo "<none>"
    else
        date -d "@$ts" "+%a %b %d %H:%M:%S %Y" 2>/dev/null || echo "Invalid Time ($ts)"
    fi
}

# 解析 Bitmap Features
parse_features() {
    local val=$1
    local type=$2 # compat, incompat, ro_compat
    local output=""
    
    # 这里只列举了常见特性，完整的列表在 ext4_fs.h 中
    if [ "$type" == "compat" ]; then
        [ $((val & 0x4)) -ne 0 ] && output="$output has_journal"
        [ $((val & 0x8)) -ne 0 ] && output="$output ext_attr"
        [ $((val & 0x10)) -ne 0 ] && output="$output resize_inode"
        [ $((val & 0x20)) -ne 0 ] && output="$output dir_index"
    elif [ "$type" == "incompat" ]; then
        [ $((val & 0x2)) -ne 0 ] && output="$output filetype"
        [ $((val & 0x4)) -ne 0 ] && output="$output needs_recovery"
        [ $((val & 0x40)) -ne 0 ] && output="$output extent"
        [ $((val & 0x80)) -ne 0 ] && output="$output 64bit"
        [ $((val & 0x200)) -ne 0 ] && output="$output flex_bg"
    elif [ "$type" == "ro_compat" ]; then
        [ $((val & 0x1)) -ne 0 ] && output="$output sparse_super"
        [ $((val & 0x2)) -ne 0 ] && output="$output large_file"
        [ $((val & 0x8)) -ne 0 ] && output="$output huge_file"
        [ $((val & 0x10)) -ne 0 ] && output="$output uninit_bg"
        [ $((val & 0x20)) -ne 0 ] && output="$output dir_nlink"
        [ $((val & 0x40)) -ne 0 ] && output="$output extra_isize"
    fi
    
    echo "$output"
}

# --- 开始解析 ---

echo "-----------------------------------------------------------------"

# 1. Magic Number (0x38, 2 bytes)
MAGIC=$(get_val 56 2)
MAGIC_HEX=$(printf "0x%X" "$MAGIC")

# 输出 Magic Number
if [ "$MAGIC" -eq 61267 ]; then # 0xEF53
    echo "Filesystem magic number:  0xEF53"
else
    echo -e "\033[31mFilesystem magic number:  $MAGIC_HEX (CORRUPT! Should be 0xEF53)\033[0m"
    echo -e "\033[31mCRITICAL: The superblock at this offset seems invalid.\033[0m"
    if [ "$SB_BLOCK_NUM" -ne 0 ]; then
        echo "Tip: Verify the block number and block size (default 4096)."
    fi
fi

# 2. Volume Name (0x78, 16 bytes)
VOL_NAME=$(get_string 120 16)
VOL_NAME=$(echo "$VOL_NAME" | xargs) # 确保去除所有首尾空白符
[ -z "$VOL_NAME" ] && VOL_NAME="<none>"
echo "Filesystem volume name:   $VOL_NAME"

# 3. Last Mounted Path (0x88, 64 bytes) - 修正解析后清理
LAST_MNT_PATH=$(get_string 136 64)
LAST_MNT_PATH=$(echo "$LAST_MNT_PATH" | xargs) # 确保去除所有首尾空白符
[ -z "$LAST_MNT_PATH" ] && LAST_MNT_PATH="<none>"
echo "Last mounted on:          $LAST_MNT_PATH"

# 4. UUID
UUID=$(get_uuid)
echo "Filesystem UUID:          $UUID"

# 5. Revision (0x4c, 4 bytes)
REV=$(get_val 76 4)
if [ "$REV" -eq 1 ]; then
    REV_STR="1 (dynamic)"
elif [ "$REV" -eq 0 ]; then
    REV_STR="0 (original)"
else
    REV_STR="$REV (unknown/corrupt)"
fi
echo "Filesystem revision #:    $REV_STR"

# 6. Features
F_COMPAT=$(get_val 92 4)
F_INCOMPAT=$(get_val 96 4)
F_RO_COMPAT=$(get_val 100 4)

FEAT_STR=""
FEAT_STR="$FEAT_STR $(parse_features $F_COMPAT "compat")"
FEAT_STR="$FEAT_STR $(parse_features $F_INCOMPAT "incompat")"
FEAT_STR="$FEAT_STR $(parse_features $F_RO_COMPAT "ro_compat")"

# 去除首尾空格
FEAT_STR=$(echo "$FEAT_STR" | xargs)
echo "Filesystem features:      $FEAT_STR"

# 7. Flags (0x24, 4 bytes) -> 简单解析常见的
FLAGS=$(get_val 36 4)
FLAG_STR=""
[ $((FLAGS & 0x1)) -ne 0 ] && FLAG_STR="$FLAG_STR signed_directory_hash"
[ $((FLAGS & 0x2)) -ne 0 ] && FLAG_STR="$FLAG_STR unsigned_directory_hash"
[ $((FLAGS & 0x4)) -ne 0 ] && FLAG_STR="$FLAG_STR test_dev_code"
echo "Filesystem flags:         $FLAG_STR"

# 8. State (0x3a, 2 bytes)
STATE=$(get_val 58 2)
if [ "$STATE" -eq 1 ]; then STATE_STR="clean";
elif [ "$STATE" -eq 2 ]; then STATE_STR="with errors";
else STATE_STR="unknown ($STATE)"; fi
echo "Filesystem state:         $STATE_STR"

# 9. Errors Behavior (0x3b, 2 bytes)
ERRS=$(get_val 59 2)
if [ "$ERRS" -eq 1 ]; then ERR_STR="Continue";
elif [ "$ERRS" -eq 2 ]; then ERR_STR="Remount-ro";
elif [ "$ERRS" -eq 3 ]; then ERR_STR="Panic";
else ERR_STR="Unknown ($ERRS)"; fi
echo "Errors behavior:          $ERR_STR"

# 10. OS Type (0x48, 4 bytes) - 修正位置，原为63(0x3F)错误
OS=$(get_val 72 4)
if [ "$OS" -eq 0 ]; then OS_STR="Linux";
elif [ "$OS" -eq 1 ]; then OS_STR="Hurd";
else OS_STR="Other ($OS)"; fi
echo "Filesystem OS type:       $OS_STR"

# 11. Inode Count (0x00, 4 bytes)
I_COUNT=$(get_val 0 4)
echo "Inode count:              $I_COUNT"
if [ "$I_COUNT" -eq 0 ]; then echo -e "\033[33m  -> WARNING: Inode count is 0, possible corruption.\033[0m"; fi

# 12. Block Count (0x04, 4 bytes)
B_COUNT=$(get_val 4 4)
echo "Block count:              $B_COUNT"

# 13. Reserved Block Count (0x08, 4 bytes)
R_B_COUNT=$(get_val 8 4)
echo "Reserved block count:     $R_B_COUNT"

# 14. Free Blocks (0x0c, 4 bytes)
F_BLOCKS=$(get_val 12 4)
echo "Free blocks:              $F_BLOCKS"

# 15. Free Inodes (0x10, 4 bytes)
F_INODES=$(get_val 16 4)
echo "Free inodes:              $F_INODES"

# 16. First Block (0x14, 4 bytes)
FIRST_BLOCK=$(get_val 20 4)
echo "First block:              $FIRST_BLOCK"

# 17. Block Size (0x18, 4 bytes) -> 实际上存储的是 log2(block_size) - 10
LOG_BLOCK_SIZE=$(get_val 24 4)
BLOCK_SIZE=$((1024 * (2 ** LOG_BLOCK_SIZE)))
echo "Block size:               $BLOCK_SIZE"
if [ "$BLOCK_SIZE" -gt 65536 ] || [ "$BLOCK_SIZE" -lt 1024 ]; then
     echo -e "\033[33m  -> WARNING: Unusual block size, possible corruption.\033[0m"
fi

# 18. Fragment Size (0x1c, 4 bytes) -> log2(frag_size) - 10
LOG_FRAG_SIZE=$(get_val 28 4)
if [ "$LOG_FRAG_SIZE" -ge 2147483648 ]; then 
    FRAG_SIZE=1024
else
    FRAG_SIZE=$((1024 * (2 ** LOG_FRAG_SIZE)))
fi
echo "Fragment size:            $FRAG_SIZE"

# 19. Blocks per Group (0x20, 4 bytes)
B_PER_G=$(get_val 32 4)
echo "Blocks per group:         $B_PER_G"

# 20. Inodes per Group (0x28, 4 bytes)
I_PER_G=$(get_val 40 4)
echo "Inodes per group:         $I_PER_G"

# 21. Inode Blocks per Group (Calculation)
I_SIZE=$(get_val 88 2)
if [ "$BLOCK_SIZE" -gt 0 ]; then
    I_BLOCKS_PER_G=$(( (I_PER_G * I_SIZE) / BLOCK_SIZE ))
    echo "Inode blocks per group:   $I_BLOCKS_PER_G"
fi

# 22. Times - 修正偏移量
# s_mkfs_time: 0x108 (264)
MKFS_TIME=$(get_val 264 4)
echo "Filesystem created:       $(format_time $MKFS_TIME)"

# s_mtime (Last mount time): 0x2C (44)
MNT_TIME=$(get_val 44 4)
echo "Last mount time:          $(format_time $MNT_TIME)"

# s_wtime (Last write time): 0x30 (48)
WR_TIME=$(get_val 48 4)
echo "Last write time:          $(format_time $WR_TIME)"

# s_lastcheck: 0x40 (64)
CHK_TIME=$(get_val 64 4)
echo "Last checked:             $(format_time $CHK_TIME)"

# s_first_error_time: 0x198 (408)
ERR_TIME=$(get_val 408 4)
echo "First error time:         $(format_time $ERR_TIME)"

# 23. Mount Counts
MNT_COUNT=$(get_val 52 2) # Offset 0x34
MAX_MNT_COUNT=$(get_val 54 2) # Offset 0x36

if [ "$MAX_MNT_COUNT" -eq 65535 ]; then MAX_MNT_COUNT="-1"; fi

echo "Mount count:              $MNT_COUNT"
echo "Maximum mount count:      $MAX_MNT_COUNT"
