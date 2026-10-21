#ifndef RGB_INDICATOR_H
#define RGB_INDICATOR_H

#include <stdint.h>
#include "rgb.h"
#include "UserConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RGB_BLUE                        rgb_from_code(0x00ff)
#define RGB_BLUE_DIM                    rgb_from_code(0x0040)   /* 启动成功：蓝色低亮 */
#define RGB_GREEN                       rgb_from_code(0xff00)
#define RGB_BLACK                       rgb_from_code(0x00)
#define RGB_YELLOW                      rgb_from_code(0xffff00)
#define RGB_CYAN                        rgb_from_code(0x00ffff)
#define RGB_RED                         rgb_from_code(0xff0000)
#define RGB_WHITE                       rgb_from_code(0xffffff)

/**
 * @brief 初始化 RGB 灯带与状态机任务（上电白慢闪）
 * @return led_strip_init 返回值（0 成功）
 */
int rgbIndicatorInit(void);

/** 初始化 SNR9912 灯效 UART（应在 GAT 复位完成后再调用，与拆分前顺序一致） */
void rgbSnrUartInit(void);

/**
 * @brief 设置 RGB 灯颜色
 * @param color RGB 颜色值
 * @return 成功返回 0，失败返回非 0
 */
int setRGBColor(rgb_t color);

/** GAT 短帧解析完成时驱动灯效（A0/A3/A4 等） */
void rgbSmOnGatDone(uint8_t gatCmd);

/** 本机向 GTA 发送 TTS（DF A0）时切到绿色呼吸 */
void rgbSmOnTtsSend(void);

/** SNR9912 帧 F4 A1 xx FB 中的命令字节 */
void rgbSmApplySnrCmd(uint8_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* RGB_INDICATOR_H */
