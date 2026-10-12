#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
合并 ESP32 烧录文件脚本
将多个独立的 bin 文件合并为一个完整的烧录文件
"""

import os
import sys
import struct

# 定义文件路径和地址映射
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
    """
    合并多个 bin 文件为一个完整的烧录文件
    
    Args:
        output_file: 输出文件名
    """
    # 创建全零的 Flash 镜像
    flash_image = bytearray(FLASH_SIZE)
    
    print("开始合并 bin 文件...")
    print(f"Flash 大小: {FLASH_SIZE // 1024 // 1024} MB")
    print("-" * 60)
    
    # 读取并写入每个文件
    for file_path, address in FLASH_FILES:
        if not os.path.exists(file_path):
            print(f"错误: 文件不存在 - {file_path}")
            return False
        
        with open(file_path, "rb") as f:
            data = f.read()
            file_size = len(data)
            
            # 检查地址和大小是否超出 Flash 范围
            if address + file_size > FLASH_SIZE:
                print(f"错误: 文件 {file_path} 超出 Flash 范围")
                print(f"  地址: 0x{address:X}, 大小: {file_size} 字节")
                print(f"  结束地址: 0x{address + file_size:X}, Flash 大小: 0x{FLASH_SIZE:X}")
                return False
            
            # 将文件数据写入 Flash 镜像
            flash_image[address:address + file_size] = data
            
            print(f"✓ {file_path}")
            print(f"  地址: 0x{address:08X} (偏移: {address // 1024} KB)")
            print(f"  大小: {file_size:,} 字节 ({file_size // 1024} KB)")
            print(f"  结束: 0x{address + file_size:08X}")
            print()
    
    # 写入合并后的文件
    with open(output_file, "wb") as f:
        f.write(flash_image)
    
    output_size = os.path.getsize(output_file)
    print("-" * 60)
    print(f"合并完成!")
    print(f"输出文件: {output_file}")
    print(f"文件大小: {output_size:,} 字节 ({output_size // 1024 // 1024} MB)")
    print()
    print("烧录命令:")
    print(f'  esptool.py -p COM32 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \\')
    print(f'    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 {output_file}')
    
    return True


def main():
    """主函数"""
    if len(sys.argv) > 1:
        output_file = sys.argv[1]
    else:
        output_file = "merged_firmware.bin"
    
    # 检查当前目录
    if not os.path.exists("build"):
        print("错误: 未找到 build 目录，请先编译项目")
        print("  运行: idf.py build")
        return 1
    
    if merge_bin_files(output_file):
        return 0
    else:
        return 1


if __name__ == "__main__":
    sys.exit(main())

