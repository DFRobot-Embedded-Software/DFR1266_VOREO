#!/bin/bash
# ESP32 鍚堝苟鐑у綍鑴氭湰
# 浣跨敤鏂规硶: ./flash_merged.sh [COM绔彛] [娉㈢壒鐜嘳
# 渚嬪: ./flash_merged.sh /dev/ttyUSB0 460800

COM_PORT=${1:-COM32}
BAUD_RATE=${2:-460800}
FLASH_SIZE=16MB
FLASH_MODE=dio
FLASH_FREQ=80m

echo "========================================"
echo "ESP32 鍚堝苟鐑у綍鑴氭湰"
echo "========================================"
echo "COM 绔彛: $COM_PORT"
echo "娉㈢壒鐜? $BAUD_RATE"
echo "========================================"
echo ""

# 妫€鏌?Python 鐜
if ! command -v python3 &> /dev/null && ! command -v python &> /dev/null; then
    echo "閿欒: 鏈壘鍒?Python锛岃鍏堝畨瑁?Python 鎴栨縺娲?ESP-IDF 鐜"
    exit 1
fi

# 妫€鏌ュ悎骞舵枃浠舵槸鍚﹀瓨鍦?if [ ! -f "merged_firmware.bin" ]; then
    echo "鍚堝苟鏂囦欢涓嶅瓨鍦紝寮€濮嬪悎骞?.."
    echo ""
    python3 merge_bin.py merged_firmware.bin || python merge_bin.py merged_firmware.bin
    if [ $? -ne 0 ]; then
        echo ""
        echo "閿欒: 鍚堝苟澶辫触"
        exit 1
    fi
    echo ""
fi

# 妫€鏌?esptool.py
if ! command -v esptool.py &> /dev/null; then
    echo "閿欒: 鏈壘鍒?esptool.py"
    echo "璇风‘淇濆凡婵€娲?ESP-IDF 鐜"
    exit 1
fi

echo "寮€濮嬬儳褰?.."
echo ""

# 鐑у綍鍚堝苟鍚庣殑鏂囦欢
esptool.py -p "$COM_PORT" -b "$BAUD_RATE" \
    --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode "$FLASH_MODE" --flash_freq "$FLASH_FREQ" --flash_size "$FLASH_SIZE" \
    0x0 merged_firmware.bin

if [ $? -ne 0 ]; then
    echo ""
    echo "閿欒: 鐑у綍澶辫触"
    exit 1
fi

echo ""
echo "========================================"
echo "鐑у綍瀹屾垚!"
echo "========================================"

