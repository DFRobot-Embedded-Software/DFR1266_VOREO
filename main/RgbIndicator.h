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

/** 圆环灯带：12 颗 WS2812，挂在 RGB_CIRCLE_GPIO */
#define RGB_CIRCLE_LED_COUNT            12
/** 每次仅点亮连续 3 颗（中心 ±1），供 DOA 方向指示 */
#define RGB_CIRCLE_LIT_COUNT            3
/**
 * LED0 相对 DOA 0°（+x 轴）的安装偏置（度，逆时针为正）。
 * 若灯带接线起点与麦阵 0° 不一致，改此值对齐。
 */
#define RGB_CIRCLE_ANGLE_OFFSET_DEG     0.0f

/**
 * @brief 初始化状态灯 + 圆环灯带与状态机任务
 * @return 0 成功，非 0 失败
 */
int rgbIndicatorInit(void);

/** 初始化 SNR9912 灯效 UART（应在 GAT 复位完成后再调用，与拆分前顺序一致） */
void rgbSnrUartInit(void);

/**
 * @brief 设置状态 RGB 灯颜色（单颗，RGB_GPIO）
 * @param color RGB 颜色值
 * @return 成功返回 0，失败返回非 0
 */
int setRGBColor(rgb_t color);

/**
 * @brief 熄灭圆环全部 12 颗灯
 * @return 成功返回 0，失败返回非 0
 */
int rgbCircleClear(void);

/**
 * @brief 按灯珠索引点亮连续 3 颗白灯（center-1, center, center+1），其余熄灭
 * @param center 中心灯索引 0..11（自动取模）
 * @return 成功返回 0，失败返回非 0
 */
int rgbCircleSetByIndex(uint8_t center);

/**
 * @brief 按 DOA 方位角点亮连续 3 颗白灯（供后续 DOA 调用）
 * @param angle_deg 方位角（度，任意实数；内部归一化到 [0,360)）
 *                  约定与 DOA 一致：0°=+x，逆时针，90°=+y
 * @return 成功返回 0，失败返回非 0
 *
 * 映射：360°/12=30° 一格；取最近灯珠为中心，再亮两侧共 3 颗。
 */
int rgbCircleSetByAngle(float angle_deg);

/** GAT 短帧解析完成时驱动灯效（A0/A3/A4/AC 等） */
void rgbSmOnGatDone(uint8_t gatCmd);

/** 本机向 GTA 发送 TTS（DF A0）时切到绿色呼吸 */
void rgbSmOnTtsSend(void);

/** SNR9912 帧 F4 A1 xx FB 中的命令字节 */
void rgbSmApplySnrCmd(uint8_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* RGB_INDICATOR_H */
