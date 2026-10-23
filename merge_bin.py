#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
合并 ESP32 烧录文件脚本
将多个独立的 bin 文件合并为一个完整的烧录文件
"""

import os
import sys

# 格式: (文件路径, 起始地址)
FLASH_FILES = [
    ("build/bootloader/bootloader.bin", 0x0),
    ("build/partition_table/partition-table.bin", 0x8000),
    ("build/DFR1266_VOREO.bin", 0x10000),
    ("build/srmodels/srmodels.bin", 0x610000),
]

# Flash 大小 (16MB)
FLASH_SIZE = 16 * 1024 * 1024  # 0x1000000


def merge_bin_files(output_file="merged_firmware.bin"):
    flash_image = bytearray(FLASH_SIZE)

    print("开始合并 bin 文件...")
    print(f"Flash 大小: {FLASH_SIZE // 1024 // 1024} MB")
    print("-" * 60)

    for file_path, address in FLASH_FILES:
        if not os.path.exists(file_path):
            print(f"错误: 文件不存在 - {file_path}")
            return False

        with open(file_path, "rb") as f:
            data = f.read()
            file_size = len(data)

            if address + file_size > FLASH_SIZE:
                print(f"错误: 文件 {file_path} 超出 Flash 范围")
                print(f"  地址: 0x{address:X}, 大小: {file_size} 字节")
                print(f"  结束地址: 0x{address + file_size:X}, Flash 大小: 0x{FLASH_SIZE:X}")
                return False

            flash_image[address:address + file_size] = data

            print(f"OK {file_path}")
            print(f"  地址: 0x{address:08X} (偏移: {address // 1024} KB)")
            print(f"  大小: {file_size:,} 字节 ({file_size // 1024} KB)")
            print(f"  结束: 0x{address + file_size:08X}")
            print()

    with open(output_file, "wb") as f:
        f.write(flash_image)

    output_size = os.path.getsize(output_file)
    print("-" * 60)
    print("合并完成!")
    print(f"输出文件: {output_file}")
    print(f"文件大小: {output_size:,} 字节 ({output_size // 1024 // 1024} MB)")
    print()
    print("烧录命令:")
    print(f'  esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \\')
    print(f'    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 {output_file}')

    return True


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "merged_firmware.bin"
    ok = merge_bin_files(out)
    sys.exit(0 if ok else 1)
