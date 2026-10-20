#!/bin/bash
# ESP32 合并烧录脚本
# 使用方法: ./flash_merged.sh [COM端口] [波特率]
# 例如: ./flash_merged.sh /dev/ttyUSB0 460800

COM_PORT=${1:-COM32}
BAUD_RATE=${2:-460800}
FLASH_SIZE=4MB
FLASH_MODE=dio
FLASH_FREQ=80m

echo "========================================"
echo "ESP32 合并烧录脚本"
echo "========================================"
echo "COM 端口: $COM_PORT"
echo "波特率: $BAUD_RATE"
echo "========================================"
echo ""

# 检查 Python 环境
if ! command -v python3 &> /dev/null && ! command -v python &> /dev/null; then
    echo "错误: 未找到 Python，请先安装 Python 或激活 ESP-IDF 环境"
    exit 1
fi

# 检查合并文件是否存在
if [ ! -f "merged_firmware.bin" ]; then
    echo "合并文件不存在，开始合并..."
    echo ""
    python3 merge_bin.py merged_firmware.bin || python merge_bin.py merged_firmware.bin
    if [ $? -ne 0 ]; then
        echo ""
        echo "错误: 合并失败"
        exit 1
    fi
    echo ""
fi

# 检查 esptool.py
if ! command -v esptool.py &> /dev/null; then
    echo "错误: 未找到 esptool.py"
    echo "请确保已激活 ESP-IDF 环境"
    exit 1
fi

echo "开始烧录..."
echo ""

# 烧录合并后的文件
esptool.py -p "$COM_PORT" -b "$BAUD_RATE" \
    --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode "$FLASH_MODE" --flash_freq "$FLASH_FREQ" --flash_size "$FLASH_SIZE" \
    0x0 merged_firmware.bin

if [ $? -ne 0 ]; then
    echo ""
    echo "错误: 烧录失败"
    exit 1
fi

echo ""
echo "========================================"
echo "烧录完成!"
echo "========================================"

