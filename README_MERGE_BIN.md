# ESP32 鍥轰欢鍚堝苟鐑у綍璇存槑

## 姒傝堪

鏈伐鍏风敤浜庡皢澶氫釜鐙珛鐨?bin 鏂囦欢鍚堝苟涓轰竴涓畬鏁寸殑鐑у綍鏂囦欢锛岀畝鍖栫儳褰曡繃绋嬨€?
## 鏂囦欢璇存槑

- `merge_bin.py` - Python 鍚堝苟鑴氭湰
- `flash_merged.bat` - Windows 鎵瑰鐞嗙儳褰曡剼鏈?- `flash_merged.sh` - Linux/Mac Shell 鐑у綍鑴氭湰

## 浣跨敤鏂规硶

### 鏂规硶 1: 浣跨敤鍚堝苟鑴氭湰锛堟帹鑽愶級

#### Windows:

```batch
# 1. 鍏堢紪璇戦」鐩?idf.py build

# 2. 鍚堝苟鏂囦欢
python merge_bin.py merged_firmware.bin

# 3. 鐑у綍锛堜娇鐢ㄦ壒澶勭悊鑴氭湰锛?flash_merged.bat COM32 460800

# 鎴栬€呯洿鎺ヤ娇鐢?esptool.py 鐑у綍
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 ^
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 merged_firmware.bin
```

#### Linux/Mac:

```bash
# 1. 鍏堢紪璇戦」鐩?idf.py build

# 2. 鍚堝苟鏂囦欢
python3 merge_bin.py merged_firmware.bin

# 3. 鐑у綍锛堜娇鐢?Shell 鑴氭湰锛?chmod +x flash_merged.sh
./flash_merged.sh /dev/ttyUSB0 460800

# 鎴栬€呯洿鎺ヤ娇鐢?esptool.py 鐑у綍
esptool.py -p /dev/ttyUSB0 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 merged_firmware.bin
```

### 鏂规硶 2: 浣跨敤 esptool.py 鐨?merge_bin 鍛戒护锛圗SP-IDF 5.1+锛?
```bash
# 鍚堝苟鏂囦欢
esptool.py --chip esp32s3 merge_bin \
    -o merged_firmware.bin \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/DFR1266_VOREO.bin \
    0x610000 build/srmodels/srmodels.bin

# 鐑у綍
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 merged_firmware.bin
```

## 鏂囦欢鍦板潃鏄犲皠

| 鏂囦欢 | 鍦板潃 | 璇存槑 |
|------|------|------|
| bootloader.bin | 0x0 | Bootloader |
| partition-table.bin | 0x8000 | 鍒嗗尯琛?|
| DFR1266_VOREO.bin | 0x10000 | 搴旂敤绋嬪簭 |
| srmodels.bin | 0x610000 | 璇煶妯″瀷锛圫PIFFS 鍒嗗尯锛?|

## 娉ㄦ剰浜嬮」

1. **缂栬瘧椤哄簭**: 蹇呴』鍏堣繍琛?`idf.py build` 鐢熸垚鎵€鏈?bin 鏂囦欢
2. **鏂囦欢璺緞**: 纭繚鎵€鏈?bin 鏂囦欢閮藉湪 `build` 鐩綍涓?3. **Flash 澶у皬**: 褰撳墠閰嶇疆涓?16MB锛屽闇€淇敼璇风紪杈?`merge_bin.py`
4. **鍦板潃鍐茬獊**: 鍚堝苟鑴氭湰浼氳嚜鍔ㄦ鏌ュ湴鍧€鏄惁瓒呭嚭 Flash 鑼冨洿

## 鏁呴殰鎺掗櫎

### 閿欒: 鏂囦欢涓嶅瓨鍦?- 纭繚宸茶繍琛?`idf.py build` 缂栬瘧椤圭洰
- 妫€鏌ユ枃浠惰矾寰勬槸鍚︽纭?
### 閿欒: 鍦板潃瓒呭嚭鑼冨洿
- 妫€鏌ュ垎鍖鸿〃閰嶇疆
- 纭 Flash 澶у皬璁剧疆姝ｇ‘

### 閿欒: 鏈壘鍒?esptool.py
- 纭繚宸叉縺娲?ESP-IDF 鐜
- Windows: 杩愯 `get_idf.bat`
- Linux/Mac: 杩愯 `. $HOME/esp/esp-idf/export.sh`

## 鍘熷鐑у綍鍛戒护锛堝弬鑰冿級

```bash
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 bootloader/bootloader.bin \
    0x10000 DFR1266_VOREO.bin \
    0x8000 partition_table/partition-table.bin \
    0x610000 srmodels/srmodels.bin
```

鍚堝苟鍚庣畝鍖栦负锛?
```bash
esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 merged_firmware.bin
```

