#include "RgbIndicator.h"

#include "PeripheralCode.h"
#include "led_strip.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "RGB_INDICATOR"

/* 开机等待 GAT_CMD_OPEN 超时后红闪再复位 */
#define RGB_BOOT_OPEN_TIMEOUT_MS        100000  /* 100s内未收到FD 00 A3 FF则红闪5s后复位 */
#define RGB_BOOT_FAIL_BLINK_MS          5000    /* 5s */
#define RGB_BOOT_FAIL_BLINK_HALF_MS     200     /* 0.2s */

static led_strip_t strip = {
    .type = LED_STRIP_WS2812,
    .length = 1,
    .gpio = RGB_GPIO,
    .buf = NULL,
#ifdef LED_STRIP_BRIGHTNESS
    .brightness = 25,
#endif
};

/**
 * RGB 指示灯状态机
 *
 *   BOOTING ──A3──► STANDBY ◄──A4── (仅 SNR 未接管时)
 *      │              │
 *      │              ├─ A0 ────────► LISTENING（正在听，紫色呼吸）
 *      │              ├─ 发 DF A0 TTS ► TTS（绿色呼吸）
 *      │              ├─ SNR 0x01 ──► SNR_YELLOW，接管，忽略 GTA 灯效
 *      │              ├─ SNR 0x02 ──► SNR_GREEN_TWICE，退出接管 ──► STANDBY
 *      │              ├─ SNR 0x03 ──► SNR_RED_TWICE，退出接管 ──► STANDBY
 *      │              └─ SNR 0x55/04/05 ► 对应灯效（不改接管标志）
 *      ├─ 启动完成前忽略 SNR / TTS 灯效
 *      └─ 100s 无 A3 ──► BOOT_FAIL（红闪 5s）──► esp_restart()
 */
typedef enum {
    RGB_ST_BOOTING = 0,
    RGB_ST_STANDBY,
    RGB_ST_LISTENING,
    RGB_ST_TTS,
    RGB_ST_SNR_YELLOW,
    RGB_ST_SNR_GREEN_TWICE,
    RGB_ST_SNR_RED_TWICE,
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
    case GAT_CMD_ASR_TIMEOUT:
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

int rgbIndicatorInit(void)
{
    led_strip_install();
    int ret = led_strip_init(&strip);
    xTaskCreate(rgbSmTask, "rgbSmTask", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "RGB indicator initialized");
    return ret;
}
