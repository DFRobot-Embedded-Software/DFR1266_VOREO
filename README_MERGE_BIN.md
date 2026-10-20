# ESP32 固件合并烧录说明

## 概述

本工具用于将多个独立的 bin 文件合并为一个完整的烧录文件，简化烧录过程。

当前配置：**4MB Flash**，已去除 `model` / srmodels（无 ESP32 本地唤醒/AFE）。

## 文件说明

- `merge_bin.py` - Python 合并脚本
- `flash_merged.bat` - Windows 批处理烧录脚本
- `flash_merged.sh` - Linux/Mac Shell 烧录脚本

## 使用方法

### 方法 1: 使用合并脚本（推荐）

#### Windows:

```batch
# 1. 先编译项目
idf.py build

# 2. 合并文件
python merge_bin.py merged_firmware.bin

# 3. 烧录（使用批处理脚本）
flash_merged.bat COM32 460800

# 或者直接使用 esptool.py 烧录
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 ^
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 merged_firmware.bin
```

#### Linux/Mac:

```bash
# 1. 先编译项目
idf.py build

# 2. 合并文件
python3 merge_bin.py merged_firmware.bin

# 3. 烧录（使用 Shell 脚本）
chmod +x flash_merged.sh
./flash_merged.sh /dev/ttyUSB0 460800

# 或者直接使用 esptool.py 烧录
esptool.py -p /dev/ttyUSB0 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 merged_firmware.bin
```

### 方法 2: 使用 esptool.py 的 merge_bin 命令（ESP-IDF 5.1+）

```bash
# 合并文件
esptool.py --chip esp32s3 merge_bin \
    -o merged_firmware.bin \
    --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/DFR1266_VOREO.bin

# 烧录
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 merged_firmware.bin
```

## 文件地址映射

| 文件 | 地址 | 说明 |
|------|------|------|
| bootloader.bin | 0x0 | Bootloader |
| partition-table.bin | 0x8000 | 分区表 |
| DFR1266_VOREO.bin | 0x10000 | 应用程序 |

## 注意事项

1. **编译顺序**: 必须先运行 `idf.py build` 生成所有 bin 文件
2. **文件路径**: 确保所有 bin 文件都在 `build` 目录下
3. **Flash 大小**: 当前配置为 **4MB**，如需修改请编辑 `merge_bin.py` / `partitions.csv` / `sdkconfig`
4. **地址冲突**: 合并脚本会自动检查地址是否超出 Flash 范围
5. **换料**: 若板子仍是 16MB 芯片，也可按 4MB 镜像烧录（只用前 4MB）；量产 4MB 料号需与分区一致

## 故障排除

### 错误: 文件不存在
- 确保已运行 `idf.py build` 编译项目
- 检查文件路径是否正确

### 错误: 地址超出范围
- 检查分区表配置
- 确认 Flash 大小设置正确

### 错误: 未找到 esptool.py
- 确保已激活 ESP-IDF 环境
- Windows: 运行 `get_idf.bat`
- Linux/Mac: 运行 `. $HOME/esp/esp-idf/export.sh`

## 原始烧录命令（参考）

```bash
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 DFR1266_VOREO.bin
```

合并后简化为：

```bash
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0 merged_firmware.bin
```
