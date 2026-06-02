## 📄 `dump_inode.sh` - Readme

### 目的

`dump_inode.sh` 是一个 **ext2/3/4 文件系统分析工具**，用于从裸设备中精确地提取并以十六进制（Hex）格式展示指定 Inode 的原始数据结构。这对于文件系统恢复、底层数据结构分析或教学非常有用。

### 功能

  * **参数校验**：检查设备路径和 Inode 号是否提供，并要求 **Root 权限** 访问裸设备。
  * **信息获取**：使用 `tune2fs` 和 `debugfs` 工具获取文件系统的块大小（Block Size）、Inode 大小（Inode Size）以及目标 Inode 的精确物理位置（块号和块内偏移）。
  * **精确转储**：计算 Inode 在设备上的绝对字节偏移量，然后使用 `dd` 命令配合 `iflag=skip_bytes` 和 `hexdump -C`，精确读取并以 Hex 格式显示整个 Inode 结构体的内容。

### 用法示例

```bash
# 需要 root 权限运行
sudo ./dump_inode.sh <设备路径> <Inode号>
# 示例：转储 /dev/sda1 上的 Inode 12345
sudo ./dump_inode.sh /dev/sda1 12345
```

-----

## 📄 `raw_dump_fs.sh` - Readme

### 目的

`raw_dump_fs.sh` 是一个 **ext2/3/4 超级块（Superblock）解析工具**。它直接从裸设备中读取超级块的原始 1024 字节数据，并对其进行详细的字段解析和输出，帮助用户检查文件系统的核心状态和配置信息，尤其适用于检查损坏的超级块。

### 功能

  * **多目标支持**：默认解析主超级块（偏移量 1024 字节），但也支持通过参数指定备份超级块的块号和块大小进行解析。
  * **原始数据读取**：使用 `hexdump` 读取超级块的 1024 字节原始数据。
  * **详细字段解析**：脚本包含辅助函数来处理 **Little Endian (小端序) 转换**、UUID 格式化、时间戳格式化以及特征位图（Feature Bitmap）的解析。
  * **关键信息输出**：清晰展示魔数（Magic Number）、Inode 数量、块大小、文件系统状态、特性标志（Features）和挂载时间等关键信息。

### 用法示例

```bash
# 1. 解析主超级块 (Primary Superblock)
sudo ./raw_dump_fs.sh <设备路径>
sudo ./raw_dump_fs.sh /dev/vda1

# 2. 解析备份超级块 (Backup Superblock)
# 假设备份在块 32768，块大小为 4096 字节
sudo ./raw_dump_fs.sh /dev/vda1 32768 4096
```

## 📄 `group_desc_free_block_num.sh` - 块组空闲块数查询脚本

### 🎯 目的 (Purpose)

该脚本用于直接从 **ext2/3/4 文件系统**的裸设备文件中读取指定**块组 (Block Group)** 的**空闲块数量 (Free Block Count)**。它通过解析超级块（Superblock）信息来确定块组描述符表（GDT）的偏移，进而提取目标块组的 32 位空闲块计数。

### 🛠 工作原理简述

1. 读取超级块中的 `s_log_block_size` 和 `s_desc_size`，以确定文件系统的**块大小**和**组描述符大小**。

2. 根据块大小计算**GDT 的起始偏移量**。

3. 计算目标**组描述符**在 GDT 中的**精确字节偏移量**。

4. 从该偏移量处读取低 16 位 (`lo`) 和高 16 位 (`hi`) 的空闲块计数，并将它们合并为最终的 32 位数值。

### 🖥 用法 (Usage)

您需要以 root 权限运行此脚本，并提供**设备文件路径**和要查询的**组号**。
sudo ./group_desc_free_block_num.sh <设备文件路径> <组号>


#### 示例

查询 `/dev/vda1` 设备上**第 0 组**的空闲块数量：

sudo ./group_desc_free_block_num.sh /dev/vda1 0


### 结果输出

脚本将输出目标块组的空闲块总数，并列出计算过程中使用的关键参数：

group 0: free_blocks = 12345 (lo=12345 hi=0 desc_size=64 block_size=4096 desc_offset=4096)
