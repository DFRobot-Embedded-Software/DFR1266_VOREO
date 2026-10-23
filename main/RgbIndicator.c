#include "RgbIndicator.h"

#include "PeripheralCode.h"
#include "led_strip.h"
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "RGB_INDICATOR"

/* 开机等待 GAT_CMD_OPEN 超时后红闪再复位 */
#define RGB_BOOT_OPEN_TIMEOUT_MS        100000  /* 100s内未收到GTA正确初始化回包则红灯闪5s后复位 */
#define RGB_BOOT_FAIL_BLINK_MS          5000    /* 5s */
#define RGB_BOOT_FAIL_BLINK_HALF_MS     200     /* 0.2s */

/* 状态指示灯：单颗，RMT_CHANNEL_0 */
static led_strip_t strip = {
    .type = LED_STRIP_WS2812,
    .length = 1,
    .gpio = RGB_GPIO,
    .channel = RMT_CHANNEL_0,
    .buf = NULL,
#ifdef LED_STRIP_BRIGHTNESS
    .brightness = 25,
#endif
};

/* DOA 圆环：12 颗白灯，RMT_CHANNEL_1，与状态灯分通道 */
static led_strip_t circleStrip = {
    .type = LED_STRIP_WS2812,
    .length = RGB_CIRCLE_LED_COUNT,
    .gpio = RGB_CIRCLE_GPIO,
    .channel = RMT_CHANNEL_1,
    .buf = NULL,
#ifdef LED_STRIP_BRIGHTNESS
    .brightness = 40,
#endif
};

static bool circleReady = false;
/** 上次按角度点亮的中心灯珠；-1 表示尚未设置（清屏后也会复位） */
static int s_circleLastCenter = -1;

/**
 * RGB 指示灯状态机
 *
 *   BOOTING ──A3──► STANDBY ◄──A4/AC── (仅 SNR 未接管时；AC=TTS播完)
 *      │              │
 *      │              ├─ A0 ────────► LISTENING（正在听，紫色呼吸）
 *      │              ├─ 发 DF A0 TTS ► TTS（绿色呼吸）──AC──► STANDBY
 *      │              ├─ SNR 0x01 ──► SNR_YELLOW，接管，忽略 GTA 灯效
 *      │              ├─ SNR 0x02 ──► SNR_GREEN_TWICE，退出接管 ──► STANDBY
 *      │              ├─ SNR 0x03 ──► SNR_RED_TWICE，退出接管 ──► STANDBY
 *      │              └─ SNR 0x55/04/05 ► 对应灯效（不改接管标志）
 *      ├─ 启动完成前忽略 SNR / TTS 灯效
 *      └─ 100s 无 A3 ──► BOOT_FAIL（红闪 5s）──► esp_restart()
 */
typedef enum {
    /* 上电等待 GTA：白色慢闪 */
    RGB_ST_BOOTING = 0,
    /* 待机就绪：蓝色低亮常亮 */
    RGB_ST_STANDBY,
    /* 正在听：紫色呼吸（FD 00 A0 FF） */
    RGB_ST_LISTENING,
    /* TTS 播放中：绿色呼吸（发 DF A0）；FD 00 AC FF 后回待机 */
    RGB_ST_TTS,
    /* SNR 0x01：黄慢闪并接管 */
    RGB_ST_SNR_YELLOW,
    /* SNR 0x02：绿闪两次后回待机并退出接管 */
    RGB_ST_SNR_GREEN_TWICE,
    /* SNR 0x03/04/05：红闪两次；03 退出接管 */
    RGB_ST_SNR_RED_TWICE,
    /* 开机超时失败：红闪 5s 后复位 */
    RGB_ST_BOOT_FAIL,
} rgb_state_t;

static volatile rgb_state_t rgbState = RGB_ST_BOOTING;
static volatile uint32_t rgbSeq = 0;
static volatile bool rgbSnrOwned = false;  /* F4 A1 01 FB 接管；02/03 退出 */

static void rgbSmTransit(rgb_state_t next)
{
    rgbState = next;
    rgbSeq++;
    ESP_LOGI(TAG, "RGB SM -> %d", (int)next);
}

void rgbSmOnGatDone(uint8_t gatCmd)
{
    if (rgbState == RGB_ST_BOOT_FAIL) {
        return;
    }
    if (rgbSnrOwned) {
        return;
    }
    switch (gatCmd) {
    case GAT_CMD_WAKEUP:
        if (gatIsOpen()) {
            rgbSmTransit(RGB_ST_LISTENING);
        }
        break;
    case GAT_CMD_OPEN:
        rgbSmTransit(RGB_ST_STANDBY);
        break;
    case GAT_CMD_ASR_TIMEOUT:   /* FD 00 A4 FF：识别结束 → 待机 */
        if (gatIsOpen()) {
            rgbSmTransit(RGB_ST_STANDBY);
        }
        break;
    case GAT_CMD_TTS_DONE:      /* FD 00 AC FF：TTS 播放完成 → 待机 */
        if (gatIsOpen()) {
            rgbSmTransit(RGB_ST_STANDBY);
        }
        break;
    default:
        break;
    }
}

void rgbSmOnTtsSend(void)
{
    if (!gatIsOpen() || rgbSnrOwned || rgbState == RGB_ST_BOOTING) {
        return;
    }
    rgbSmTransit(RGB_ST_TTS);
}

void rgbSmApplySnrCmd(uint8_t cmd)
{
    if (!gatIsOpen() || rgbState == RGB_ST_BOOTING) {
        return;
    }
    switch (cmd) {
    case 0x55:
        break;
    case 0x01:
        rgbSnrOwned = true;
        rgbSmTransit(RGB_ST_SNR_YELLOW);
        break;
    case 0x02:
        rgbSnrOwned = false;
        rgbSmTransit(RGB_ST_SNR_GREEN_TWICE);
        break;
    case 0x03:
        rgbSnrOwned = false;
        rgbSmTransit(RGB_ST_SNR_RED_TWICE);
        break;
    case 0x04:
    case 0x05:
        rgbSmTransit(RGB_ST_SNR_RED_TWICE);
        break;
    default:
        ESP_LOGW(TAG, "未知RGB命令: 0x%02X", cmd);
        return;
    }
    ESP_LOGI(TAG, "RGB SNR cmd: 0x%02X owned=%d", cmd, (int)rgbSnrOwned);
}

static void rgbSmTask(void *arg)
{
    uint32_t seen = 0;
    const TickType_t bootStart = xTaskGetTickCount();

    while (1) {
        rgb_state_t st = rgbState;
        uint32_t seq = rgbSeq;

        switch (st) {
        case RGB_ST_BOOTING:
            if (!gatIsOpen() &&
                (xTaskGetTickCount() - bootStart) >= pdMS_TO_TICKS(RGB_BOOT_OPEN_TIMEOUT_MS)) {
                ESP_LOGE(TAG, "GAT_CMD_OPEN timeout %d ms", RGB_BOOT_OPEN_TIMEOUT_MS);
                rgbSmTransit(RGB_ST_BOOT_FAIL);
                break;
            }
            seen = seq;
            setRGBColor(RGB_WHITE);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (rgbSeq != seen) {
                break;
            }
            setRGBColor(RGB_BLACK);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case RGB_ST_BOOT_FAIL: {
            TickType_t failStart = xTaskGetTickCount();
            while ((xTaskGetTickCount() - failStart) < pdMS_TO_TICKS(RGB_BOOT_FAIL_BLINK_MS)) {
                setRGBColor(RGB_RED);
                vTaskDelay(pdMS_TO_TICKS(RGB_BOOT_FAIL_BLINK_HALF_MS));
                setRGBColor(RGB_BLACK);
                vTaskDelay(pdMS_TO_TICKS(RGB_BOOT_FAIL_BLINK_HALF_MS));
            }
            ESP_LOGE(TAG, "boot fail, esp_restart()");
            esp_restart();
            break;
        }

        case RGB_ST_STANDBY:
            setRGBColor(RGB_BLUE_DIM);
            seen = seq;
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case RGB_ST_LISTENING: {
            seen = seq;
            for (int v = 8; v <= 255; v += 8) {
                if (rgbSeq != seen) {
                    break;
                }
                setRGBColor((rgb_t){ .r = (uint8_t)v, .g = 0, .b = (uint8_t)v });
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            for (int v = 255; v >= 8; v -= 8) {
                if (rgbSeq != seen) {
                    break;
                }
                setRGBColor((rgb_t){ .r = (uint8_t)v, .g = 0, .b = (uint8_t)v });
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            break;
        }

        case RGB_ST_TTS: {
            seen = seq;
            for (int g = 8; g <= 255; g += 8) {
                if (rgbSeq != seen) {
                    break;
                }
                setRGBColor((rgb_t){ .r = 0, .g = (uint8_t)g, .b = 0 });
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            for (int g = 255; g >= 8; g -= 8) {
                if (rgbSeq != seen) {
                    break;
                }
                setRGBColor((rgb_t){ .r = 0, .g = (uint8_t)g, .b = 0 });
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            break;
        }

        case RGB_ST_SNR_YELLOW:
            seen = seq;
            setRGBColor(RGB_YELLOW);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (rgbSeq != seen) {
                break;
            }
            setRGBColor(RGB_BLACK);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case RGB_ST_SNR_GREEN_TWICE:
        case RGB_ST_SNR_RED_TWICE: {
            rgb_t color = (st == RGB_ST_SNR_GREEN_TWICE) ? RGB_GREEN : RGB_RED;
            seen = seq;
            for (int i = 0; i < 2; i++) {
                if (rgbSeq != seen) {
                    break;
                }
                setRGBColor(color);
                vTaskDelay(pdMS_TO_TICKS(200));
                if (rgbSeq != seen) {
                    break;
                }
                setRGBColor(RGB_BLACK);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (rgbSeq == seen) {
                rgbSmTransit(RGB_ST_STANDBY);
            }
            break;
        }

        default:
            setRGBColor(RGB_BLACK);
            seen = seq;
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }
}

static void snr9912UartTask(void *arg)
{
    uint8_t buf[64];
    int state = 0;
    uint8_t data = 0;

    while (1) {
        int n = uart_read_bytes(SNR9912_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (state == 0) {
                if (b == 0xF4) {
                    state = 1;
                }
            } else if (state == 1) {
                if (b == 0xA1) {
                    state = 2;
                } else {
                    state = (b == 0xF4) ? 1 : 0;
                }
            } else if (state == 2) {
                data = b;
                state = 3;
            } else {
                if (b == 0xFB) {
                    rgbSmApplySnrCmd(data);
                }
                state = (b == 0xF4) ? 1 : 0;
            }
        }
    }
}

void rgbSnrUartInit(void)
{
    if (!uart_is_driver_installed(SNR9912_UART_PORT)) {
        uart_config_t config = {
            .baud_rate = SNR9912_UART_BAUDRATE,
            .data_bits = UART_DATA_BITS,
            .stop_bits = UART_STOP_BITS,
            .parity = UART_PARITY,
            .flow_ctrl = UART_FLOW_CTRL,
            .source_clk = UART_SOURCE_CLK,
        };
        ESP_ERROR_CHECK(uart_driver_install(SNR9912_UART_PORT, UART_BUF_SIZE, 0, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(SNR9912_UART_PORT, &config));
    }
    ESP_ERROR_CHECK(uart_set_pin(SNR9912_UART_PORT, SNR9912_UART_TX_GPIO, SNR9912_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(snr9912UartTask, "snr9912UartTask", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "SNR9912 UART GPIO%d/GPIO%d %d", SNR9912_UART_TX_GPIO, SNR9912_UART_RX_GPIO,
             SNR9912_UART_BAUDRATE);
}

int setRGBColor(rgb_t color)
{
    led_strip_set_pixel(&strip, 0, color);
    return led_strip_flush(&strip);
}

int rgbCircleClear(void)
{
    if (!circleReady) {
        return -1;
    }
    s_circleLastCenter = -1;
    led_strip_fill(&circleStrip, 0, RGB_CIRCLE_LED_COUNT, RGB_BLACK);
    return led_strip_flush(&circleStrip);
}

int rgbCircleSetByIndex(uint8_t center)
{
    if (!circleReady) {
        return -1;
    }
    center = (uint8_t)(center % RGB_CIRCLE_LED_COUNT);

    led_strip_fill(&circleStrip, 0, RGB_CIRCLE_LED_COUNT, RGB_BLACK);
    /* 连续 3 颗：center-1, center, center+1（环形） */
    for (int i = -1; i <= 1; i++) {
        int idx = ((int)center + i + RGB_CIRCLE_LED_COUNT) % RGB_CIRCLE_LED_COUNT;
        led_strip_set_pixel(&circleStrip, (size_t)idx, RGB_WHITE);
    }
    int ret = led_strip_flush(&circleStrip);
    if (ret == 0) {
        s_circleLastCenter = (int)center;
    }
    return ret;
}

int rgbCircleSetByAngle(float angle_deg)
{
    /* 归一化到 [0, 360) */
    while (angle_deg < 0.0f) {
        angle_deg += 360.0f;
    }
    while (angle_deg >= 360.0f) {
        angle_deg -= 360.0f;
    }

    const float sector = 360.0f / (float)RGB_CIRCLE_LED_COUNT; /* 30° */
    float adjusted = angle_deg - RGB_CIRCLE_ANGLE_OFFSET_DEG;
    while (adjusted < 0.0f) {
        adjusted += 360.0f;
    }
    while (adjusted >= 360.0f) {
        adjusted -= 360.0f;
    }
    /* 四舍五入到最近灯珠 */
    int center = (int)floorf((adjusted + sector * 0.5f) / sector) % RGB_CIRCLE_LED_COUNT;
    if (center == s_circleLastCenter) {
        return 0; /* 扇区未变，跳过刷灯 */
    }
    return rgbCircleSetByIndex((uint8_t)center);
}

int rgbIndicatorInit(void)
{
    led_strip_install();
    int ret = led_strip_init(&strip);
    if (ret != 0) {
        ESP_LOGE(TAG, "status LED init fail: %d", ret);
        return ret;
    }

    ret = led_strip_init(&circleStrip);
    if (ret != 0) {
        ESP_LOGE(TAG, "circle LED init fail: %d", ret);
        return ret;
    }
    circleReady = true;
    rgbCircleClear();

    xTaskCreate(rgbSmTask, "rgbSmTask", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "RGB indicator+circle(%d) initialized GPIO%d",
             RGB_CIRCLE_LED_COUNT, RGB_CIRCLE_GPIO);
    return 0;
}
