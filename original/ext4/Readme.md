### ext4全盘扫extent信息恢复已删除大文件

快速使用手册：这里假设/dev/nvme2n1设备是文件被删了待恢复的设备
#### 1. 扫描
```
./ext_scan /dev/nvme2n1 &> ext_scan_out.txt
```

#### 2. 查看都找到了哪些extent信息（按大小倒序）
```
./ext_util.py stat ext_scan_out.txt
```
示例输出：
```
------------------------------
extent:34006          start_addr:196577280      len:55032085       (214969.08MB)
extent:31495695       start_addr:178184192      len:375056         (1465.06MB)
extent:31495756       start_addr:319920128      len:371620         (1451.64MB)
extent:31495741       start_addr:327340032      len:363286         (1419.09MB)
extent:31495706       start_addr:172165120      len:360258         (1407.26MB)
extent:31495746       start_addr:171780096      len:264047         (1031.43MB)
extent:91280866       start_addr:266670080      len:252458         (986.16MB)
extent:31495681       start_addr:174192640      len:246071         (961.21MB)
extent:31495680       start_addr:177377280      len:238560         (931.88MB)
extent:24162129       start_addr:216152064      len:232539         (908.36MB)
--------------------------------
```

#### 3. 选择一个extent, 比如extent:31495695，恢复到recover.dat文件
```
./ext_util.py dump ext_scan_out.txt 31495695 /dev/nvme2n1 recover.dat
```
