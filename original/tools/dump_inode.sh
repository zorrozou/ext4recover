#!/bin/bash

# 检查参数个数
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <device> <inode_number>"
    echo "Example: $0 /dev/sda1 12345"
    exit 1
fi

DEVICE=$1
INODE=$2

# 检查是否为 root 用户 (读取裸设备需要权限)
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script requires root privileges to access raw devices."
    exit 1
fi

# 检查设备是否存在
if [ ! -b "$DEVICE" ]; then
    echo "Error: Device $DEVICE not found."
    exit 1
fi

echo "Target Device: $DEVICE"
echo "Target Inode : $INODE"
echo "----------------------------------------"

# 1. 获取 Block Size 和 Inode Size
# 使用 tune2fs -l 读取超级块信息
# 2>/dev/null 用于屏蔽可能出现的版本警告
BLOCK_SIZE=$(tune2fs -l "$DEVICE" 2>/dev/null | grep "Block size" | awk '{print $3}')
INODE_SIZE=$(tune2fs -l "$DEVICE" 2>/dev/null | grep "Inode size" | awk '{print $3}')

if [ -z "$BLOCK_SIZE" ] || [ -z "$INODE_SIZE" ]; then
    echo "Error: Failed to read filesystem superblock. Is it a valid ext2/3/4 partition?"
    exit 1
fi

echo "Block Size   : $BLOCK_SIZE bytes"
echo "Inode Size   : $INODE_SIZE bytes"

# 2. 使用 debugfs 获取 Inode 的具体位置
# imap 输出示例: "Inode 12 is part of block group 0 located at block 265, offset 0x0b00"
# 我们需要提取 block 后的数字 和 offset 后的十六进制数
IMAP_OUTPUT=$(debugfs -R "imap <$INODE>" "$DEVICE" 2>/dev/null)

if [[ $IMAP_OUTPUT == *"File not found"* ]] || [[ -z "$IMAP_OUTPUT" ]]; then
    echo "Error: Inode $INODE not found or invalid."
    exit 1
fi

# 解析 Block 号 (去除逗号)
BLOCK_NUM=$(echo "$IMAP_OUTPUT" | grep -oP 'located at block \K[0-9]+')

# 解析 Offset (十六进制)
OFFSET_HEX=$(echo "$IMAP_OUTPUT" | grep -oP 'offset \K0x[0-9a-fA-F]+')

if [ -z "$BLOCK_NUM" ] || [ -z "$OFFSET_HEX" ]; then
    echo "Error: Failed to parse debugfs output."
    echo "Debugfs output: $IMAP_OUTPUT"
    exit 1
fi

# 将十六进制偏移转换为十进制
OFFSET_DEC=$(printf "%d" "$OFFSET_HEX")

echo "Located at Block: $BLOCK_NUM"
echo "Offset in Block : $OFFSET_HEX (Hex) -> $OFFSET_DEC (Dec)"

# 3. 计算绝对字节偏移量 (Byte Offset)
# 使用 bc 进行大数运算防止溢出 (虽然 bash 64位整数通常够用，但 bc 更稳妥)
BYTE_OFFSET=$(echo "$BLOCK_NUM * $BLOCK_SIZE + $OFFSET_DEC" | bc)

echo "Absolute Byte Offset: $BYTE_OFFSET"
echo "----------------------------------------"
echo "Dumping raw inode data..."
echo ""

# 4. 使用 dd 和 hexdump 输出
# iflag=skip_bytes 允许我们直接按字节跳过，而不是按块跳过，这样更精确且不用算 skip block count
dd if="$DEVICE" bs=1 count="$INODE_SIZE" skip="$BYTE_OFFSET" iflag=skip_bytes 2>/dev/null | hexdump -C
echo  "dd if="$DEVICE" bs=1 count="$INODE_SIZE" skip="$BYTE_OFFSET" iflag=skip_bytes 2>/dev/null | hexdump -C"
