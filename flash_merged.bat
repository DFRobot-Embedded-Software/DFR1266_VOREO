@echo off
REM ESP32 合并烧录脚本
REM 使用方法: flash_merged.bat [COM端口] [波特率]
REM 例如: flash_merged.bat COM32 460800

set COM_PORT=COM32
set BAUD_RATE=460800
set FLASH_SIZE=16MB
set FLASH_MODE=dio
set FLASH_FREQ=80m

REM 如果提供了参数，使用参数
if not "%1"=="" set COM_PORT=%1
if not "%2"=="" set BAUD_RATE=%2

echo ========================================
echo ESP32 合并烧录脚本
echo ========================================
echo COM 端口: %COM_PORT%
echo 波特率: %BAUD_RATE%
echo ========================================
echo.

REM 检查 Python 环境
python --version >nul 2>&1
if errorlevel 1 (
    echo 错误: 未找到 Python，请先安装 Python 或激活 ESP-IDF 环境
    pause
    exit /b 1
)

REM 检查合并文件是否存在
if not exist "merged_firmware.bin" (
    echo 合并文件不存在，开始合并...
    echo.
    python merge_bin.py merged_firmware.bin
    if errorlevel 1 (
        echo.
        echo 错误: 合并失败
        pause
        exit /b 1
    )
    echo.
)

REM 检查 esptool.py
where esptool.py >nul 2>&1
if errorlevel 1 (
    echo 错误: 未找到 esptool.py
    echo 请确保已激活 ESP-IDF 环境
    pause
    exit /b 1
)

echo 开始烧录...
echo.

REM 烧录合并后的文件
esptool.py -p %COM_PORT% -b %BAUD_RATE% ^
    --before default_reset --after hard_reset --chip esp32s3 ^
    write_flash --flash_mode %FLASH_MODE% --flash_freq %FLASH_FREQ% --flash_size %FLASH_SIZE% ^
    0x0 merged_firmware.bin

if errorlevel 1 (
    echo.
    echo 错误: 烧录失败
    pause
    exit /b 1
)

echo.
echo ========================================
echo 烧录完成!
echo ========================================
pause

