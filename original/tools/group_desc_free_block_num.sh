# 设备文件和组号
DEV=$1
GROUP=$2   # 修改为你要查询的组号

# 读 s_log_block_size (superblock 在偏移 1024，s_log_block_size 在 superblock 内偏移 0x18)
s_log_block_size=$(od -An -t u4 -j $((1024 + 0x18)) -N4 "$DEV" | tr -d ' ')
block_size=$((1024 << s_log_block_size))

# 计算 GDT 起始偏移
if [ "$block_size" -eq 1024 ]; then
  gdt_offset=2048
else
  gdt_offset=$block_size
fi

# 读取 s_desc_size (superblock 内偏移 0xFE)
desc_size=$(od -An -t u2 -j $((1024 + 0xFE)) -N2 "$DEV" | tr -d ' ')
if [ -z "$desc_size" ] || [ "$desc_size" -eq 0 ]; then
  desc_size=32
fi

# 计算目标组描述符起始偏移
desc_offset=$(( gdt_offset + GROUP * desc_size ))

# 读取 lo / hi（little-endian 16-bit）
lo=$(hexdump -s $((desc_offset + 0xC)) -n 2 -e '1/2 "%u\n"' "$DEV")
if [ "$desc_size" -gt 44 ]; then
  hi=$(hexdump -s $((desc_offset + 0x2C)) -n 2 -e '1/2 "%u\n"' "$DEV")
else
  hi=0
fi

# 合并成 32-bit 数
free_blocks=$(( lo + (hi << 16) ))

echo "group $GROUP: free_blocks = $free_blocks (lo=$lo hi=$hi desc_size=$desc_size block_size=$block_size desc_offset=$desc_offset)"
