#include "PeripheralCode.h"
#include "led_strip.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "soc/uart_periph.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"
#include "hal/gpio_hal.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "driver/i2c_slave.h"
#include <string.h>
#include "esp_log.h"
#include "UserCode.h"

#define TAG "USER_PERIPHERAL"

/* 上电后直接向 GTA 发送音量/语速/TTS 三条命令做联调；测完改 0 */
#define GTA_HOST_CMD_SELFTEST           1

// CRC16-CCITT 多项式 (0x1021)
#define CRC16_POLYNOMIAL 0x1021
#define CRC16_INITIAL_VALUE 0xFFFF

// GAT数据链表实例
static gat_data_list_t *gatDataList = NULL;

static int commMode = UART_MODE;
extern float angle; 
static uint8_t asrState = 0;

static i2c_slave_dev_handle_t slave_handle = NULL;

static QueueHandle_t slaveReceiveQueue = NULL;

/* 寄存器邮箱静态映像：先就绪再被读，避免 on_request 赶写竞态 */
static uint8_t i2cSlaveSendData[I2C_APP_FRAME_MAX];
static volatile uint16_t i2cSlaveSendDataLen = 0;
static volatile uint16_t i2cTxDataOffset = 0;
static volatile uint8_t i2cLinkStatus = VOREO_I2C_LINK_IDLE;
static volatile uint8_t i2cMbReg = VOREO_REG_STATUS; /* 当前读指针 */
static volatile bool i2cDataArmed = false;            /* 仅在选中 DATA 后允许泵应答 */
static volatile bool i2cRxQueueOverflow = false;
static volatile bool i2cAbortReq = false;

static uint8_t i2cRxBuffer[I2C_APP_FRAME_MAX];
static volatile uint16_t i2cRxBufferLen = 0;
static volatile uint16_t i2cExpectedPacketLen = 0;

static bool isOpen = true;  //调试

typedef enum {
    I2C_SLAVE_EVT_RX,
    I2C_SLAVE_EVT_TX
} i2c_slave_evt_type_t;

typedef struct {
    i2c_slave_evt_type_t type;
    uint16_t len;
    uint8_t data[I2C_RX_EVENT_MAX];
} i2c_slave_event_t;

static void i2cResetRxAssemble(void);
static void i2cResetTx(void);
static void i2cAbortAll(void);
static void i2cMailboxPumpTx(void);

// GAT解析状态机状态（需要保持跨函数调用）
static struct {
    uint8_t cmdState;      // 当前解析状态
    uint16_t dataLen;      // 期望的数据长度（用于ASR_DATA命令）
    uint8_t lastCmd;       // 上一个命令（用于非ASR_DATA命令）
} gatParseState = {
    .cmdState = GAT_UART_CMD_HEAD0,
    .dataLen = 0,
    .lastCmd = 0
},userParseState = {
    .cmdState = USER_UART_CMD_HEAD,
    .dataLen = 0,
    .lastCmd = 0
};

static QueueHandle_t userUartQueue = NULL;
static QueueHandle_t gtaUartQueue = NULL;
static void snr9912UartInit(void);

static led_strip_t strip =
{
    .type = LED_STRIP_WS2812,
    .length = 1,
    .gpio = RGB_GPIO,
    .buf = NULL,
#ifdef LED_STRIP_BRIGHTNESS
    .brightness = 25, // 亮度范围0-255
#endif
};

/**
 * @brief 计算CRC16-CCITT校验值
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return CRC16校验值
 */
static uint16_t calculateCRC16(const uint8_t *data, size_t len)
{
    uint16_t crc = CRC16_INITIAL_VALUE;
    
    for (size_t i = 0; i < len; i++) {
        // 先转换为uint16_t再左移，避免符号扩展问题
        crc ^= ((uint16_t)data[i]) << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = ((crc << 1) ^ CRC16_POLYNOMIAL) & 0xFFFF;
            } else {
                crc = (crc << 1) & 0xFFFF;
            }
        }
    }
    
    return crc;
}
/**
 * @brief 发送数据包（自动添加包头和CRC16校验）
 * @param data 要发送的数据缓冲区
 * @param len 数据长度
 * @param cmd 命令类型
 */
static void sendBuf(uint8_t *data, size_t len, uint8_t cmd)
{
    // 限制最大数据长度，避免栈溢出
    const size_t MAX_PACKET_SIZE = GAT_DATA_BUFFER_MAX_SIZE + 6;
    if (len > GAT_DATA_BUFFER_MAX_SIZE) {
        ESP_LOGE(TAG, "数据长度超出限制: %d > %d", len, GAT_DATA_BUFFER_MAX_SIZE);
        return;
    }
    
    // 使用固定大小的缓冲区，避免变长数组导致的栈溢出
    static uint8_t send_buffer[GAT_DATA_BUFFER_MAX_SIZE + 6];
    size_t total_len = len + 6;
    
    send_buffer[0] = USER_UART_CMD_HEAD;
    send_buffer[1] = (len >> 8) & 0xff;
    send_buffer[2] = len & 0xff;    
    send_buffer[3] = cmd;
    memcpy(send_buffer + 4, data, len);
    uint16_t crc = calculateCRC16(send_buffer, len + 4);
    send_buffer[len + 4] = (uint8_t)(crc >> 8);
    send_buffer[len + 5] = (uint8_t)(crc & 0xFF);
    
    if(commMode == UART_MODE){
        // 使用非阻塞方式写入，避免阻塞导致看门狗超时
        int written = uart_write_bytes(USERUART_PORT, send_buffer, total_len);
        if (written < 0) {
            ESP_LOGE(TAG, "UART写入失败");
        } else if ((size_t)written < total_len) {
            ESP_LOGW(TAG, "UART部分写入: %d/%d", written, total_len);
        }
        // 刷新发送缓冲区，确保数据发送完成
        uart_wait_tx_done(USERUART_PORT, pdMS_TO_TICKS(100));
    }else if(commMode == I2C_MODE){
        if (total_len > I2C_APP_FRAME_MAX) {
            ESP_LOGE(TAG, "I2C reply too long: %u > %u", (unsigned)total_len, I2C_APP_FRAME_MAX);
            i2cLinkStatus = VOREO_I2C_LINK_ERROR;
            i2cSlaveSendDataLen = 0;
            i2cTxDataOffset = 0;
            return;
        }
        /* 先拷完整应答再置 READY——STATUS/DATA 只读静态映像 */
        memcpy(i2cSlaveSendData, send_buffer, total_len);
        i2cSlaveSendDataLen = (uint16_t)total_len;
        i2cTxDataOffset = 0;
        i2cDataArmed = false; /* 须再次选 DATA 才允许泵送 */
        i2cMbReg = VOREO_REG_STATUS;
        i2cLinkStatus = VOREO_I2C_LINK_READY;
        ESP_LOGI(TAG, "I2C mailbox READY len=%u", (unsigned)total_len);
    }
}

/** 主机→GTA：DF + cmd + payload + FF */
static void sendBufToGAT(uint8_t cmd, const uint8_t *data, size_t len)
{
    static uint8_t send_buffer[GAT_DATA_BUFFER_MAX_SIZE + 3];
    if (len > GAT_DATA_BUFFER_MAX_SIZE) {
        ESP_LOGE(TAG, "sendBufToGAT: payload too long (%u)", (unsigned)len);
        return;
    }

    size_t total_len = len + 3;
    send_buffer[0] = GAT_HOST_HEAD;
    send_buffer[1] = cmd;
    if (data && len > 0) {
        memcpy(send_buffer + 2, data, len);
    }
    send_buffer[2 + len] = GAT_CMD_END;

    ESP_LOGI(TAG, "sendBufToGAT TX: %02X %02X ... %02X (len=%u)",
             send_buffer[0], send_buffer[1], send_buffer[total_len - 1], (unsigned)total_len);

    int written = uart_write_bytes(GTA_UART_PORT, send_buffer, total_len);
    if (written < 0 || (size_t)written < total_len) {
        ESP_LOGW(TAG, "sendBufToGAT: write %d/%u cmd=0x%02X", written, (unsigned)total_len, cmd);
    }
    uart_wait_tx_done(GTA_UART_PORT, pdMS_TO_TICKS(200));
}
/**
 * @brief 解析用户UART数据（状态机实现）
 * @param data 接收到的数据缓冲区
 * @param len 数据长度
 */
static void userParseData(const uint8_t *data, size_t len)
{
    uint8_t sendData = 0;
    static uint8_t sendBufData[2];
    if (!data || len == 0) {
        ESP_LOGW(TAG, "无效的用户数据：空指针或长度为零");
        return;
    }

    for(int i = 0; i < len; i++)
    {
        if(userParseState.cmdState == USER_UART_CMD_HEAD){
            if(data[i] == USER_UART_CMD_HEAD)
            {
                userParseState.cmdState = UART_CMD_LEN_H;
            }
        }else if(userParseState.cmdState == UART_CMD_LEN_H){
            userParseState.dataLen = (uint16_t)data[i] << 8;
            userParseState.cmdState = UART_CMD_LEN_L;
        }else if(userParseState.cmdState == UART_CMD_LEN_L){
            userParseState.dataLen |= (uint16_t)data[i];
            userParseState.cmdState = UART_CMD_CMD;
        }else if(userParseState.cmdState == UART_CMD_CMD){
            switch(data[i])
            {
                // ESP_LOGI(TAG, "USER command received: 0x%02X", data[i]);
                case USER_UART_CMD_QUERY_TEXT:
                    //ESP_LOGI(TAG, "USER query text command received");
                    userParseState.cmdState = USER_UART_CMD_HEAD;
                    
                    // 检查是否有完整的USER_UART_CMD_SEND_TEXT节点
                    sendData = 0;
                    if (gatDataList) {
                        gat_data_node_t *current = gatDataList->head;
                        while (current) {
                            if (current->cmd == GAT_CMD_ASR_DATA && 
                                gatDataNodeIsComplete(current) && 
                                current->data && 
                                current->received_len > 0) {
                                sendData = 1;
                                break;
                            }
                            current = current->next;
                        }
                    }
                    sendBuf(&sendData, 1, USER_UART_CMD_QUERY_TEXT);
                    break;
                case USER_UART_CMD_GET_TEXT:
                    ESP_LOGI(TAG, "USER get text command received");
                    //setRGBColor(RGB_BLACK);
                    userParseState.cmdState = USER_UART_CMD_HEAD;
                    // 从链表中查找完整的USER_UART_CMD_SEND_TEXT节点
                    if (gatDataList) {
                        gat_data_node_t *node = NULL;
                        // 遍历链表查找USER_UART_CMD_SEND_TEXT命令的完整节点
                        gat_data_node_t *current = gatDataList->head;
                        while (current) {
                            if (current->cmd == GAT_CMD_ASR_DATA && 
                                gatDataNodeIsComplete(current) && 
                                current->data && 
                                current->received_len > 0) {
                                node = current;
                                break;
                            }
                            current = current->next;
                        }
                        
                        if (node) {
                            // 发送数据（sendBuf会自动添加包头和CRC16）
                            sendBuf(node->data, node->received_len, USER_UART_CMD_GET_TEXT);
                            ESP_LOGI(TAG, "Sent text data, length: %d", node->received_len);
                            // 从链表中移除已发送的节点（gatDataListRemove内部会释放节点）
                            gatDataListRemove(gatDataList, node);
                        } else {
                            // 没有可用的文本数据，发送空数据
                            uint8_t emptyData[1] = {0};
                            sendBuf(emptyData, 1, USER_UART_CMD_GET_TEXT);
                            ESP_LOGW(TAG, "No text data available to send");
                        }
                    } else {
                        // 链表未初始化，发送空数据
                        uint8_t emptyData[1] = {0};
                        sendBuf(emptyData, 1, USER_UART_CMD_GET_TEXT);
                        ESP_LOGE(TAG, "GAT data list not initialized");
                    }
                    break;
                case USER_UART_CMD_SEND_TEXT:
                    ESP_LOGI(TAG, "USER send text command received, expected data length: %d", userParseState.dataLen);
                    /* TTS 单包最大 128B，收齐后再转 GTA */
                    if (userParseState.dataLen > VOREO_TTS_PAYLOAD_MAX) {
                        ESP_LOGW(TAG, "USER TTS payload %u > %u, reject",
                                 (unsigned)userParseState.dataLen, VOREO_TTS_PAYLOAD_MAX);
                        userParseState.cmdState = USER_UART_CMD_HEAD;
                        userParseState.dataLen = 0;
                        break;
                    }
                    if (userParseState.dataLen > 0) {
                        if (gatDataList) {
                            gat_data_node_t *node = gatDataNodeCreate(USER_UART_CMD_SEND_TEXT, userParseState.dataLen);
                            if (node) {
                                gatDataListAdd(gatDataList, node);
                                userParseState.cmdState = USER_UART_CMD_DATA;
                                ESP_LOGI(TAG, "Created USER text data node, expected length: %d", userParseState.dataLen);
                            } else {
                                ESP_LOGE(TAG, "Failed to create USER text data node, reset state");
                                userParseState.cmdState = USER_UART_CMD_HEAD;
                                userParseState.dataLen = 0;
                            }
                        } else {
                            ESP_LOGE(TAG, "GAT data list not initialized, reset state");
                            userParseState.cmdState = USER_UART_CMD_HEAD;
                            userParseState.dataLen = 0;
                        }
                    } else {
                        // 没有数据，直接返回
                        ESP_LOGW(TAG, "USER send text command with zero data length");
                        userParseState.cmdState = USER_UART_CMD_HEAD;
                    }
                    break;
                case USER_UART_CMD_ANGLE:
                    userParseState.cmdState = USER_UART_CMD_HEAD;   
                    sendBufData[0] = ((uint16_t)angle >> 8) & 0xff;
                    sendBufData[1] = ((uint16_t)angle & 0xff);
                    sendBuf(sendBufData, 2, USER_UART_CMD_ANGLE);   
                    break;
                case USER_UART_CMD_BEGIN:
                    ESP_LOGI(TAG, "USER begin command received");
                    userParseState.cmdState = USER_UART_CMD_HEAD;
                    if(isOpen)
                    {
                        ESP_LOGI(TAG, "USER begin command received, but already open");
                        sendData = 1;
                    }else{
                        ESP_LOGI(TAG, "USER begin command received, not open");
                        sendData = 0;
                    }
                    
                    sendBuf(&sendData, 1, USER_UART_CMD_BEGIN);
                    break;
                case USER_UART_CMD_SET_ASR:
                    ESP_LOGI(TAG, "USER set ASR command received, expected data length: %d", userParseState.dataLen);
                    // 如果有数据需要接收，创建节点并进入数据接收状态
                    if (userParseState.dataLen > 0) {
                        if (gatDataList) {
                            gat_data_node_t *node = gatDataNodeCreate(USER_UART_CMD_SET_ASR, userParseState.dataLen);
                            if (node) {
                                gatDataListAdd(gatDataList, node);
                                userParseState.cmdState = USER_UART_CMD_DATA;
                                ESP_LOGI(TAG, "Created USER ASR data node, expected length: %d", userParseState.dataLen);
                            } else {
                                ESP_LOGE(TAG, "Failed to create USER ASR data node, reset state");
                                userParseState.cmdState = USER_UART_CMD_HEAD;
                                userParseState.dataLen = 0;
                            }
                        } else {
                            ESP_LOGE(TAG, "GAT data list not initialized, reset state");
                            userParseState.cmdState = USER_UART_CMD_HEAD;
                            userParseState.dataLen = 0;
                        }
                    } else {
                        // 没有数据，直接返回
                        ESP_LOGW(TAG, "USER set ASR command with zero data length");
                        userParseState.cmdState = USER_UART_CMD_HEAD;
                    }
                    break;
                case USER_UART_CMD_TTS_SPEED:
                    ESP_LOGI(TAG, "USER TTS speed command, dataLen=%d", userParseState.dataLen);
                    userParseState.lastCmd = USER_UART_CMD_TTS_SPEED;
                    if (userParseState.dataLen == 1) {
                        userParseState.cmdState = USER_UART_CMD_DATA;
                    } else {
                        ESP_LOGW(TAG, "USER TTS speed expects 1 byte, got len=%d", userParseState.dataLen);
                        userParseState.cmdState = USER_UART_CMD_HEAD;
                        userParseState.dataLen = 0;
                        userParseState.lastCmd = 0;
                    }
                    break;
                case USER_UART_CMD_TTS_VOLUME:
                    ESP_LOGI(TAG, "USER TTS volume command, dataLen=%d", userParseState.dataLen);
                    userParseState.lastCmd = USER_UART_CMD_TTS_VOLUME;
                    if (userParseState.dataLen == 1) {
                        userParseState.cmdState = USER_UART_CMD_DATA;
                    } else {
                        ESP_LOGW(TAG, "USER TTS volume expects 1 byte, got len=%d", userParseState.dataLen);
                        userParseState.cmdState = USER_UART_CMD_HEAD;
                        userParseState.dataLen = 0;
                        userParseState.lastCmd = 0;
                    }
                    break;
                case USER_UART_CMD_GET_ASR_STATE:
                    ESP_LOGI(TAG, "USER get ASR state command received");
                    userParseState.cmdState = USER_UART_CMD_HEAD;
                    sendBufData[0] = (uint8_t)asrState;
                    sendBuf(sendBufData, 1, USER_UART_CMD_GET_ASR_STATE);
                    asrState = 0;
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown USER command received: 0x%02X, reset state", data[i]);
                    userParseState.cmdState = USER_UART_CMD_HEAD;
                    break;
            }
        } else if(userParseState.cmdState == USER_UART_CMD_DATA){
            // 单字节语速：立即转发到 GTA（DF A2 + speed + FF）
            if (userParseState.lastCmd == USER_UART_CMD_TTS_SPEED) {
                setGatSpeed(data[i]);
                userParseState.cmdState = USER_UART_CMD_HEAD;
                userParseState.dataLen = 0;
                userParseState.lastCmd = 0;
                continue;
            }
            // 单字节音量：立即转发到 GTA（DF A1 + volume + FF）
            if (userParseState.lastCmd == USER_UART_CMD_TTS_VOLUME) {
                setGatVolume(data[i]);
                userParseState.cmdState = USER_UART_CMD_HEAD;
                userParseState.dataLen = 0;
                userParseState.lastCmd = 0;
                continue;
            }
            // 接收数据并存储到节点中（支持 SEND_TEXT 和 SET_ASR）
            if (gatDataList) {
                gat_data_node_t *node = gatDataListGetIncomplete(gatDataList);
                if (node && (node->cmd == USER_UART_CMD_SEND_TEXT || node->cmd == USER_UART_CMD_SET_ASR)) {
                    int appended = gatDataNodeAppend(node, &data[i], 1);
                    if (appended > 0) {
                        if (gatDataNodeIsComplete(node)) {
                            ESP_LOGI(TAG, "USER text data packet complete, total length: %d", node->received_len);
                            userParseState.cmdState = USER_UART_CMD_HEAD;
                            userParseState.dataLen = 0;
                        }
                        // 如果数据未完成，继续保持在DATA状态
                    } else {
                        ESP_LOGW(TAG, "Failed to append data to USER text node, reset state");
                        userParseState.cmdState = USER_UART_CMD_HEAD;
                        userParseState.dataLen = 0;
                    }
                } else {
                    ESP_LOGW(TAG, "No incomplete USER text node found, reset state");
                    userParseState.cmdState = USER_UART_CMD_HEAD;
                    userParseState.dataLen = 0;
                }
            } else {
                ESP_LOGW(TAG, "GAT data list not initialized, reset state");
                userParseState.cmdState = USER_UART_CMD_HEAD;
                userParseState.dataLen = 0;
            }
        }
    }
}

static void i2cResetRxAssemble(void)
{
    i2cRxBufferLen = 0;
    i2cExpectedPacketLen = 0;
}

static void i2cResetTx(void)
{
    i2cSlaveSendDataLen = 0;
    i2cTxDataOffset = 0;
    i2cDataArmed = false;
    i2cLinkStatus = VOREO_I2C_LINK_IDLE;
    i2cMbReg = VOREO_REG_STATUS;
}

static void i2cAbortAll(void)
{
    i2cResetRxAssemble();
    i2cResetTx();
    ESP_LOGI(TAG, "I2C ABORT: mailbox cleared");
}

/** 邮箱 STATUS 映像（组帧未完成时为 BUSY） */
static uint8_t i2cMailboxStatus(uint16_t *outLen)
{
    *outLen = 0;
    if (i2cExpectedPacketLen > 0 && i2cRxBufferLen < i2cExpectedPacketLen) {
        return VOREO_I2C_LINK_BUSY;
    }
    if (i2cLinkStatus == VOREO_I2C_LINK_READY && i2cSlaveSendDataLen > 0) {
        *outLen = i2cSlaveSendDataLen;
        return VOREO_I2C_LINK_READY;
    }
    if (i2cLinkStatus == VOREO_I2C_LINK_ERROR) {
        return VOREO_I2C_LINK_ERROR;
    }
    if (i2cLinkStatus == VOREO_I2C_LINK_BUSY) {
        return VOREO_I2C_LINK_BUSY;
    }
    return VOREO_I2C_LINK_IDLE;
}

/** 按当前 mbReg 从静态映像泵数据到硬件 FIFO；失败也写 dummy，避免主机 SCL 死等 */
static void i2cMailboxPumpTx(void)
{
    uint32_t write_len = 0;
    uint8_t reg = i2cMbReg;
    uint8_t dummy = 0xFF;

    if (reg == VOREO_REG_STATUS || reg == VOREO_REG_LEN_H || reg == VOREO_REG_LEN_L) {
        uint16_t respLen = 0;
        uint8_t st = i2cMailboxStatus(&respLen);
        uint8_t statusBuf[3] = {
            st,
            (uint8_t)(respLen >> 8),
            (uint8_t)(respLen & 0xff)
        };
        esp_err_t err = i2c_slave_write(slave_handle, statusBuf, 3, &write_len, 0);
        if (err != ESP_OK || write_len < 3) {
            ESP_LOGW(TAG, "I2C mailbox STATUS write fail err=%s wrote=%lu",
                     esp_err_to_name(err), (unsigned long)write_len);
            i2c_slave_write(slave_handle, statusBuf, 3, &write_len, 50);
        } else {
            ESP_LOGI(TAG, "I2C mailbox TX STATUS=%u len=%u", st, respLen);
        }
        return;
    }

    if (reg == VOREO_REG_DATA) {
        /* 未显式选中 DATA 时不抽应答，避免 begin 后残留 on_request 把角度包抽空 */
        if (!i2cDataArmed || i2cLinkStatus != VOREO_I2C_LINK_READY || i2cSlaveSendDataLen == 0) {
            i2c_slave_write(slave_handle, &dummy, 1, &write_len, 50);
            return;
        }
        if (i2cTxDataOffset >= i2cSlaveSendDataLen) {
            i2cDataArmed = false;
            i2cLinkStatus = VOREO_I2C_LINK_IDLE;
            i2cMbReg = VOREO_REG_STATUS;
            i2c_slave_write(slave_handle, &dummy, 1, &write_len, 50);
            return;
        }
        uint16_t remaining = i2cSlaveSendDataLen - i2cTxDataOffset;
        uint16_t chunk = (remaining > I2C_TX_CHUNK_MAX) ? I2C_TX_CHUNK_MAX : remaining;
        esp_err_t err = i2c_slave_write(slave_handle, i2cSlaveSendData + i2cTxDataOffset,
                                        chunk, &write_len, 0);
        if (err != ESP_OK || write_len == 0) {
            ESP_LOGW(TAG, "I2C mailbox DATA write fail err=%s", esp_err_to_name(err));
            i2c_slave_write(slave_handle, i2cSlaveSendData + i2cTxDataOffset,
                            chunk, &write_len, 50);
            if (write_len == 0) {
                i2c_slave_write(slave_handle, &dummy, 1, &write_len, 50);
                i2cLinkStatus = VOREO_I2C_LINK_ERROR;
                i2cDataArmed = false;
                return;
            }
        }
        i2cTxDataOffset += (uint16_t)write_len;
        ESP_LOGI(TAG, "I2C mailbox TX DATA %lu/%u off=%u",
                 (unsigned long)write_len, i2cSlaveSendDataLen, i2cTxDataOffset);
        if (i2cTxDataOffset >= i2cSlaveSendDataLen) {
            /* 读完：IDLE，但保留缓冲直到下次 FIFO；指针回到 STATUS */
            i2cDataArmed = false;
            i2cLinkStatus = VOREO_I2C_LINK_IDLE;
            i2cMbReg = VOREO_REG_STATUS;
            i2cSlaveSendDataLen = 0;
            i2cTxDataOffset = 0;
        }
        return;
    }

    i2c_slave_write(slave_handle, &dummy, 1, &write_len, 50);
}

/**
 * @brief Full user frame: CRC then parse（sendBuf 填充邮箱后再 READY）
 */
static void i2cProcessUserFrame(const uint8_t *frame, uint16_t frameLen)
{
    if (frameLen < 3) {
        ESP_LOGW(TAG, "I2C frame too short: %u", frameLen);
        i2cLinkStatus = VOREO_I2C_LINK_ERROR;
        return;
    }
    uint16_t calculated_crc = calculateCRC16(frame, frameLen - 2);
    uint16_t received_crc = ((uint16_t)frame[frameLen - 2] << 8) | frame[frameLen - 1];
    if (calculated_crc != received_crc) {
        ESP_LOGW(TAG, "I2C CRC error calc=0x%04X recv=0x%04X", calculated_crc, received_crc);
        i2cLinkStatus = VOREO_I2C_LINK_ERROR;
        i2cSlaveSendDataLen = 0;
        return;
    }
    userParseData(frame, frameLen - 2);
}

static void i2cSlaveTask(void *pvParameters)
{
    i2c_slave_event_t event;
    TickType_t assembleStartTick = 0;

    while (1)
    {
        if (i2cRxQueueOverflow) {
            i2cRxQueueOverflow = false;
            ESP_LOGE(TAG, "I2C RX queue overflow -> ERROR");
            i2cAbortAll();
            i2cLinkStatus = VOREO_I2C_LINK_ERROR;
        }

        /* 超时按「距上一包 RX」计时，兼容主机慢分包；总空闲超过 3s 才丢弃 */
        if (i2cExpectedPacketLen > 0 && i2cRxBufferLen > 0 && i2cRxBufferLen < i2cExpectedPacketLen) {
            if (assembleStartTick == 0) {
                assembleStartTick = xTaskGetTickCount();
            } else if ((xTaskGetTickCount() - assembleStartTick) > pdMS_TO_TICKS(3000)) {
                ESP_LOGW(TAG, "I2C assemble timeout %u/%u", i2cRxBufferLen, i2cExpectedPacketLen);
                i2cResetRxAssemble();
                assembleStartTick = 0;
                i2cLinkStatus = VOREO_I2C_LINK_ERROR;
            }
        } else if (i2cExpectedPacketLen == 0 || i2cRxBufferLen == 0) {
            assembleStartTick = 0;
        }

        if (xQueueReceive(slaveReceiveQueue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (i2cAbortReq) {
            i2cAbortReq = false;
            i2cAbortAll();
        }

        if (event.type == I2C_SLAVE_EVT_TX) {
            i2cMailboxPumpTx();
            continue;
        }

        if (event.type != I2C_SLAVE_EVT_RX) {
            continue;
        }

        const uint8_t *rx = event.data;
        uint16_t rxLen = event.len;
        ESP_LOGI(TAG, "I2C RX len=%u", rxLen);
        if (rxLen == 0) {
            continue;
        }

        /* 寄存器写：首字节为 reg */
        uint8_t reg = rx[0];
        const uint8_t *payload = rx + 1;
        uint16_t payloadLen = (rxLen > 0) ? (uint16_t)(rxLen - 1) : 0;

        if (reg == VOREO_REG_CTRL) {
            if (payloadLen >= 1 && payload[0] == VOREO_REG_CTRL_ABORT) {
                i2cAbortAll();
            }
            continue;
        }

        if (reg == VOREO_REG_STATUS || reg == VOREO_REG_LEN_H || reg == VOREO_REG_LEN_L
            || reg == VOREO_REG_DATA) {
            i2cMbReg = reg;
            if (reg == VOREO_REG_DATA) {
                i2cDataArmed = true;
                if (i2cLinkStatus == VOREO_I2C_LINK_READY) {
                    i2cTxDataOffset = 0;
                }
            } else {
                i2cDataArmed = false;
            }
            ESP_LOGI(TAG, "I2C mailbox select reg=0x%02X armed=%d", reg, (int)i2cDataArmed);
            continue;
        }

        if (reg != VOREO_REG_FIFO) {
            ESP_LOGW(TAG, "I2C unknown reg 0x%02X", reg);
            continue;
        }

        /* FIFO：追加应用帧；组帧中 STATUS=BUSY */
        if (payloadLen == 0) {
            continue;
        }
        if (i2cLinkStatus == VOREO_I2C_LINK_READY && i2cTxDataOffset == 0 && i2cSlaveSendDataLen > 0) {
            /* 新请求覆盖未读完的旧应答 */
            i2cResetTx();
        }
        i2cLinkStatus = VOREO_I2C_LINK_BUSY;

        if (i2cRxBufferLen + payloadLen > I2C_APP_FRAME_MAX) {
            ESP_LOGW(TAG, "I2C FIFO overflow, reset");
            i2cResetRxAssemble();
            i2cLinkStatus = VOREO_I2C_LINK_ERROR;
            continue;
        }
        memcpy(i2cRxBuffer + i2cRxBufferLen, payload, payloadLen);
        i2cRxBufferLen += payloadLen;
        assembleStartTick = xTaskGetTickCount();

        if (i2cExpectedPacketLen == 0 && i2cRxBufferLen >= 3) {
            if (i2cRxBuffer[0] == USER_UART_CMD_HEAD) {
                uint16_t dataLen = ((uint16_t)i2cRxBuffer[1] << 8) | i2cRxBuffer[2];
                uint16_t expect = (uint16_t)(6 + dataLen);
                if (expect > I2C_APP_FRAME_MAX) {
                    ESP_LOGW(TAG, "I2C frame too large: %u", expect);
                    i2cResetRxAssemble();
                    i2cLinkStatus = VOREO_I2C_LINK_ERROR;
                    continue;
                }
                i2cExpectedPacketLen = expect;
                assembleStartTick = xTaskGetTickCount();
                ESP_LOGI(TAG, "I2C assemble expect=%u cur=%u", i2cExpectedPacketLen, i2cRxBufferLen);
            } else {
                ESP_LOGW(TAG, "I2C bad head 0x%02X", i2cRxBuffer[0]);
                i2cResetRxAssemble();
                assembleStartTick = 0;
                i2cLinkStatus = VOREO_I2C_LINK_ERROR;
            }
        }

        while (i2cExpectedPacketLen > 0 && i2cRxBufferLen >= i2cExpectedPacketLen) {
            ESP_LOGI(TAG, "I2C frame complete len=%u", i2cExpectedPacketLen);
            i2cProcessUserFrame(i2cRxBuffer, i2cExpectedPacketLen);

            uint16_t remain = i2cRxBufferLen - i2cExpectedPacketLen;
            if (remain > 0) {
                memmove(i2cRxBuffer, i2cRxBuffer + i2cExpectedPacketLen, remain);
            }
            i2cRxBufferLen = remain;
            i2cExpectedPacketLen = 0;
            assembleStartTick = 0;
            if (i2cRxBufferLen >= 3 && i2cRxBuffer[0] == USER_UART_CMD_HEAD) {
                uint16_t dataLen = ((uint16_t)i2cRxBuffer[1] << 8) | i2cRxBuffer[2];
                uint16_t expect = (uint16_t)(6 + dataLen);
                if (expect > I2C_APP_FRAME_MAX) {
                    i2cResetRxAssemble();
                    i2cLinkStatus = VOREO_I2C_LINK_ERROR;
                    break;
                }
                i2cExpectedPacketLen = expect;
                assembleStartTick = xTaskGetTickCount();
            } else if (i2cRxBufferLen > 0 && i2cRxBuffer[0] != USER_UART_CMD_HEAD) {
                i2cRxBufferLen = 0;
            }
        }
    }
}

/**
 * @brief 用户UART任务（仅处理UART数据）
 * @param pvParameters 任务参数（未使用）
 */
static void userUartTask(void *pvParameters)
{
    int dataLen = 0;
    uint8_t* dtmp = (uint8_t*) malloc(UART_READ_BUF_SIZE + 1);
    assert(dtmp);
    
    while (1)
    {
        // 只处理UART模式
        dataLen = uart_read_bytes(USERUART_PORT, dtmp, UART_READ_BUF_SIZE, 0);
        
        if (dataLen > 0) {
            // CRC16需要至少3个字节（1字节数据 + 2字节CRC）
            if (dataLen < 3) {
                ESP_LOGW(TAG, "用户UART数据太短，无法进行CRC16校验：%d 字节", dataLen);
                continue;
            }
            
            // 计算CRC16（不包括最后2个字节，它们是CRC16值）
            uint16_t calculated_crc = calculateCRC16(dtmp, dataLen - 2);
            
            // 从数据包中提取接收到的CRC16（高字节在前，低字节在后）
            uint16_t received_crc = (uint16_t)dtmp[dataLen - 2] << 8 | dtmp[dataLen - 1];
            
            // 验证CRC16
            if (calculated_crc == received_crc) {
                // 校验成功，解析数据（不包括CRC16的2个字节）
                userParseData(dtmp, dataLen - 2);
            } else {
                // 校验失败，跳过该数据包
                ESP_LOGW(TAG, "用户UART CRC16校验错误：计算值=0x%04X，接收值=0x%04X", 
                         calculated_crc, received_crc);
            }
        } 
        // 没有数据可用，让出CPU
        vTaskDelay(1);
    }
}
/**
 * @brief 解析GTA通信数据（状态机实现）
 * @param data 接收到的数据缓冲区指针
 * @param len 接收到的数据长度
 */
static void gtaParseData(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        ESP_LOGW(TAG, "无效的GTA数据：空指针或长度为零");
        return;
    }
    
    ESP_LOGI(TAG, "GTA data received, length: %d, current state: %d", len, gatParseState.cmdState);
    
    for(int i = 0; i < len; i++)
    {
        if(gatParseState.cmdState == GAT_UART_CMD_HEAD0){
            if(data[i] == GAT_CMD_HEAD0)
            {
                gatParseState.cmdState = GAT_UART_CMD_HEAD1;
            }
            // 如果不是HEAD0，继续保持在HEAD0状态，等待下一个字节
        }
        else if(gatParseState.cmdState == GAT_UART_CMD_HEAD1){
            if(data[i] == GAT_CMD_HEAD1)
            {
                gatParseState.cmdState = UART_CMD_CMD;
            }else{
                // 如果HEAD1不匹配，检查当前字节是否是新的HEAD0
                if(data[i] == GAT_CMD_HEAD0) {
                    gatParseState.cmdState = GAT_UART_CMD_HEAD1; // 可能是新的数据包开始
                } else {
                    gatParseState.cmdState = GAT_UART_CMD_HEAD0; // 重置状态
                }
            }
        }else if(gatParseState.cmdState == UART_CMD_CMD){
           switch(data[i])
           {
            case GAT_CMD_WAKEUP:
                ESP_LOGI(TAG, "GTA wakeup command received");
                //setRGBColor(RGB_GREEN);
                gatParseState.lastCmd = GAT_CMD_WAKEUP;
                gatParseState.cmdState = GAT_UART_CMD_END;
                asrState = 1;
                break;
            case GAT_CMD_DETECTED:
                ESP_LOGI(TAG, "GTA detected command received");
                //setRGBColor(RGB_BLUE);
                gatParseState.lastCmd = GAT_CMD_DETECTED;
                gatParseState.cmdState = GAT_UART_CMD_END;
                asrState = 2;
                break;
            case GAT_CMD_ASR_DATA:
                gatParseState.cmdState = UART_CMD_LEN_H;
                gatParseState.lastCmd = GAT_CMD_ASR_DATA;
                gatParseState.dataLen = 0;
                ESP_LOGI(TAG, "GTA ASR data command received");
                break;
            case GAT_CMD_READ_ERROR:
                ESP_LOGI(TAG, "GTA read error command received");
                gatParseState.lastCmd = GAT_CMD_READ_ERROR;
                gatParseState.cmdState = GAT_UART_CMD_END;
                break;
            case GAT_CMD_OPEN:
                ESP_LOGI(TAG, "GTA open command received");
                gatParseState.lastCmd = GAT_CMD_OPEN;
                gatParseState.cmdState = GAT_UART_CMD_END;
                if(isOpen)
                {
                    ESP_LOGI(TAG, "GTA open command received, already open");
                    break;
                }else{  
                    isOpen = true;
                }
                break;
            case GAT_CMD_ASR_TIMEOUT:
                ESP_LOGI(TAG, "GTA ASR timeout (0xA4)");
                gatParseState.lastCmd = GAT_CMD_ASR_TIMEOUT;
                gatParseState.cmdState = GAT_UART_CMD_END;
                break;
            case GAT_CMD_SPEED_ACK:
                ESP_LOGI(TAG, "GTA speed set ack (0xA5)");
                gatParseState.lastCmd = GAT_CMD_SPEED_ACK;
                gatParseState.cmdState = GAT_UART_CMD_END;
                break;
            case GAT_CMD_VOLUME_ACK:
                ESP_LOGI(TAG, "GTA volume set ack (0xA6)");
                gatParseState.lastCmd = GAT_CMD_VOLUME_ACK;
                gatParseState.cmdState = GAT_UART_CMD_END;
                break;
            default:
                ESP_LOGW(TAG, "Unknown GTA command received: 0x%02X, reset state", data[i]);
                gatParseState.cmdState = GAT_UART_CMD_HEAD0; // 重置状态，避免卡住
                    break;
            }
        }else if(gatParseState.cmdState == UART_CMD_LEN_H){
            gatParseState.dataLen = (uint16_t)data[i] << 8;
            gatParseState.cmdState = UART_CMD_LEN_L;
        }else if(gatParseState.cmdState == UART_CMD_LEN_L){
            gatParseState.dataLen |= (uint16_t)data[i];
            gatParseState.cmdState = GAT_UART_CMD_DATA;
            /* FD 00 A2 + len + UTF-8 + FF：每帧从头存储，上限 4KB */
            if (gatParseState.dataLen > VOREO_ASR_PAYLOAD_MAX) {
                ESP_LOGW(TAG, "ASR len %u > %u, drop frame",
                         (unsigned)gatParseState.dataLen, VOREO_ASR_PAYLOAD_MAX);
                gatParseState.cmdState = GAT_UART_CMD_HEAD0;
                gatParseState.dataLen = 0;
            } else if (gatDataList && gatParseState.dataLen > 0) {
                gatDataListRemoveAllCmd(gatDataList, GAT_CMD_ASR_DATA);
                gat_data_node_t *node = gatDataNodeCreate(GAT_CMD_ASR_DATA, gatParseState.dataLen);
                if (node) {
                    gatDataListAdd(gatDataList, node);
                    ESP_LOGI(TAG, "ASR frame restart, expected length: %d", gatParseState.dataLen);
                } else {
                    ESP_LOGE(TAG, "Failed to create GAT data node, reset state");
                    gatParseState.cmdState = GAT_UART_CMD_HEAD0;
                    gatParseState.dataLen = 0;
                }
            } else {
                ESP_LOGW(TAG, "Invalid data length or list not initialized, reset state");
                gatParseState.cmdState = GAT_UART_CMD_HEAD0;
                gatParseState.dataLen = 0;
            }
        }else if(gatParseState.cmdState == GAT_UART_CMD_DATA){
            // Get the current incomplete node and append data
            if (gatDataList) {
                gat_data_node_t *node = gatDataListGetIncomplete(gatDataList);
                if (node) {
                    int appended = gatDataNodeAppend(node, &data[i], 1);
                    if (appended > 0) {
                        if (gatDataNodeIsComplete(node)) {
                            ESP_LOGI(TAG, "GAT data packet complete, total length: %d", node->received_len);
                            gatParseState.cmdState = GAT_UART_CMD_END;
                        }
                        // 如果数据未完成，继续保持在DATA状态
                    } else {
                        ESP_LOGW(TAG, "Failed to append data to node, reset state");
                        gatParseState.cmdState = GAT_UART_CMD_HEAD0; // Reset on error
                        gatParseState.dataLen = 0;
                    }
                } else {
                    ESP_LOGW(TAG, "No incomplete node found for data, reset state");
                    gatParseState.cmdState = GAT_UART_CMD_HEAD0; // Reset on error
                    gatParseState.dataLen = 0;
                }
            } else {
                ESP_LOGW(TAG, "GAT data list not initialized, reset state");
                gatParseState.cmdState = GAT_UART_CMD_HEAD0;
                gatParseState.dataLen = 0;
            }
        }
        else if(gatParseState.cmdState == GAT_UART_CMD_END){
            if(data[i] == GAT_CMD_END)
            {
                // 数据包解析完成，重置状态准备接收下一个数据包
                ESP_LOGI(TAG, "GAT packet end received, command: 0x%02X", gatParseState.lastCmd);
                gatParseState.cmdState = GAT_UART_CMD_HEAD0;
                gatParseState.dataLen = 0;
                gatParseState.lastCmd = 0;
                // 不continue，继续处理后续字节（可能包含下一个数据包）
            }else{
                // 期望收到结束符但收到其他字节，可能是错误或下一个数据包的开始
                ESP_LOGW(TAG, "Expected END (0xFF) but received 0x%02X, reset state", data[i]);
                if(data[i] == GAT_CMD_HEAD0) {
                    // 可能是下一个数据包开始
                    gatParseState.cmdState = GAT_UART_CMD_HEAD1;
                } else {
                    gatParseState.cmdState = GAT_UART_CMD_HEAD0;
                }
            }
        }
    }
}

// ==================== GAT数据链表实现 ====================

/**
 * @brief 初始化GAT数据链表
 * @return 返回初始化后的链表指针，失败返回NULL
 */
gat_data_list_t* gatDataListInit(void)
{
    gat_data_list_t *list = (gat_data_list_t*)malloc(sizeof(gat_data_list_t));
    if (!list) {
        ESP_LOGE(TAG, "Failed to allocate GAT data list");
        return NULL;
    }
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
    return list;
}

/**
 * @brief 创建新的GAT数据节点
 * @param cmd 命令类型
 * @param data_len 期望的数据长度
 * @return 返回新节点的指针，失败返回NULL
 */
gat_data_node_t* gatDataNodeCreate(uint8_t cmd, uint16_t data_len)
{
    if (data_len == 0 || data_len > GAT_DATA_BUFFER_MAX_SIZE) {
        ESP_LOGE(TAG, "Invalid data length: %d", data_len);
        return NULL;
    }
    
    gat_data_node_t *node = (gat_data_node_t*)malloc(sizeof(gat_data_node_t));
    if (!node) {
        ESP_LOGE(TAG, "Failed to allocate GAT data node");
        return NULL;
    }
    
    node->data = (uint8_t*)malloc(data_len);
    if (!node->data) {
        ESP_LOGE(TAG, "Failed to allocate data buffer");
        free(node);
        return NULL;
    }
    
    node->cmd = cmd;
    node->data_len = data_len;
    node->received_len = 0;
    node->next = NULL;
    
    return node;
}

/**
 * @brief 向链表添加节点
 * @param list 链表指针
 * @param node 要添加的节点指针
 * @return 成功返回0，失败返回-1
 */
int gatDataListAdd(gat_data_list_t *list, gat_data_node_t *node)
{
    if (!list || !node) {
        return -1;
    }
    
    if (list->head == NULL) {
        list->head = node;
        list->tail = node;
    } else {
        list->tail->next = node;
        list->tail = node;
    }
    list->count++;
    return 0;
}

/**
 * @brief 向节点追加数据
 * @param node 目标节点指针
 * @param data 要追加的数据缓冲区
 * @param len 要追加的数据长度
 * @return 返回实际追加的字节数
 */
int gatDataNodeAppend(gat_data_node_t *node, const uint8_t *data, size_t len)
{
    if (!node || !data || len == 0) {
        return -1;
    }
    
    size_t remaining = node->data_len - node->received_len;
    size_t to_copy = (len < remaining) ? len : remaining;
    
    if (to_copy > 0) {
        memcpy(node->data + node->received_len, data, to_copy);
        node->received_len += to_copy;
    }
    
    return to_copy;
}

/**
 * @brief 检查节点数据是否接收完整
 * @param node 节点指针
 * @return 完整返回true，否则返回false
 */
bool gatDataNodeIsComplete(gat_data_node_t *node)
{
    if (!node) {
        return false;
    }
    return (node->received_len >= node->data_len);
}

/**
 * @brief 从链表中移除节点并释放内存
 * @param list 链表指针
 * @param node 要移除的节点指针
 * @return 成功返回0，失败返回-1
 */
int gatDataListRemove(gat_data_list_t *list, gat_data_node_t *node)
{
    if (!list || !node) {
        return -1;
    }
    
    if (list->head == node) {
        list->head = node->next;
        if (list->tail == node) {
            list->tail = NULL;
        }
    } else {
        gat_data_node_t *prev = list->head;
        while (prev && prev->next != node) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = node->next;
            if (list->tail == node) {
                list->tail = prev;
            }
        } else {
            return -1; // Node not found in list
        }
    }
    
    list->count--;
    gatDataNodeFree(node);
    return 0;
}

void gatDataListRemoveAllCmd(gat_data_list_t *list, uint8_t cmd)
{
    if (!list) {
        return;
    }
    gat_data_node_t *current = list->head;
    while (current) {
        gat_data_node_t *next = current->next;
        if (current->cmd == cmd) {
            gatDataListRemove(list, current);
        }
        current = next;
    }
}


/**
 * @brief 获取链表中第一个未完成的节点
 * @param list 链表指针
 * @return 返回未完成节点指针，如果没有则返回NULL
 */
gat_data_node_t* gatDataListGetIncomplete(gat_data_list_t *list)
{
    if (!list) {
        return NULL;
    }
    
    gat_data_node_t *current = list->head;
    while (current) {
        if (!gatDataNodeIsComplete(current)) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/**
 * @brief 获取链表中第一个已完成的节点
 * @param list 链表指针
 * @return 返回已完成节点指针，如果没有则返回NULL
 */
gat_data_node_t* gatDataListGetComplete(gat_data_list_t *list)
{
    if (!list) {
        return NULL;
    }
    
    gat_data_node_t *current = list->head;
    while (current) {
        if (gatDataNodeIsComplete(current)) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/**
 * @brief 释放节点内存
 * @param node 要释放的节点指针
 */
void gatDataNodeFree(gat_data_node_t *node)
{
    if (node) {
        if (node->data) {
            free(node->data);
            node->data = NULL;
        }
        free(node);
    }
}

/**
 * @brief 释放整个链表及其所有节点
 * @param list 要释放的链表指针
 */
void gatDataListFree(gat_data_list_t *list)
{
    if (!list) {
        return;
    }
    
    gat_data_node_t *current = list->head;
    while (current) {
        gat_data_node_t *next = current->next;
        gatDataNodeFree(current);
        current = next;
    }
    
    free(list);
}

/**
 * @brief 获取链表中节点的数量
 * @param list 链表指针
 * @return 返回节点数量
 */
uint32_t gatDataListGetCount(gat_data_list_t *list)
{
    if (!list) {
        return 0;
    }
    return list->count;
}

/**
 * @brief GTA UART任务（轮询模式接收数据）
 * @param pvParameters 任务参数（未使用）
 */
static void gtaUartTask(void *pvParameters)
{
    uint8_t* dtmp = (uint8_t*) malloc(UART_READ_BUF_SIZE + 1);
    assert(dtmp);
    while (1)
    {
        // 轮询模式：读取可用数据（非阻塞）
        // 最多读取UART_READ_BUF_SIZE字节，如果数据不足则立即返回
        int len = uart_read_bytes(GTA_UART_PORT, dtmp, UART_READ_BUF_SIZE, 0);
        if(len > 0)
        {
            gtaParseData(dtmp, len);
        }
        vTaskDelay(1);
    }
}

static void userTask(void *pvParameters)
{
    while (1)
    {
        if (gatDataList) {
            gat_data_node_t *node = NULL;
            // 遍历链表查找USER_UART_CMD_SEND_TEXT命令的完整节点
            gat_data_node_t *current = gatDataList->head;
            while (current) {
                if ((current->cmd == USER_UART_CMD_SEND_TEXT || current->cmd == USER_UART_CMD_SET_ASR) && 
                    gatDataNodeIsComplete(current) && 
                    current->data && 
                    current->received_len > 0) {
                    node = current;
                    break;
                }
                current = current->next;
            }

            if (node && node->cmd == USER_UART_CMD_SEND_TEXT) {
                /* 收齐一包且 ≤128B 才转发 GTA */
                if (node->received_len > VOREO_TTS_PAYLOAD_MAX) {
                    ESP_LOGE(TAG, "TTS packet %u > %u, drop",
                             (unsigned)node->received_len, VOREO_TTS_PAYLOAD_MAX);
                } else {
                    sendBufToGAT(GAT_HOST_CMD_TTS, node->data, node->received_len);
                    ESP_LOGI(TAG, "Sent TTS to GTA, length: %d", node->received_len);
                }
                gatDataListRemove(gatDataList, node);
            } else if (node && node->cmd == USER_UART_CMD_SET_ASR) {
                // ESP32 本地 ASR/AFE 已移除；唤醒词由 GAT 模组侧配置
                ESP_LOGW(TAG, "SET_ASR ignored (no local ESP32 ASR), len=%d", node->received_len);
                gatDataListRemove(gatDataList, node);
            }
        }
        vTaskDelay(1);
    }
}

/* 邮箱模型：RX/TX 一律进任务；映像已就绪，on_request 只负责泵送 */
static bool i2cSlaveReceiveDoneCb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_rx_done_event_data_t *evt_data, void *arg)
{
    (void)i2c_slave;
    QueueHandle_t receive_queue = (QueueHandle_t)arg;
    BaseType_t xTaskWoken = 0;
    size_t copy_len = evt_data->length;
    if (copy_len > I2C_RX_EVENT_MAX) {
        copy_len = I2C_RX_EVENT_MAX;
    }

    i2c_slave_event_t evt = {
        .type = I2C_SLAVE_EVT_RX,
        .len = 0,
        .data = {0},
    };
    evt.len = (uint16_t)copy_len;
    if (copy_len > 0 && evt_data->buffer) {
        memcpy(evt.data, evt_data->buffer, copy_len);
    }
    if (xQueueSendFromISR(receive_queue, &evt, &xTaskWoken) != pdTRUE) {
        i2cRxQueueOverflow = true;
    }
    return xTaskWoken;
}

static bool i2cSlaveRequestDoneCb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_request_event_data_t *evt_data, void *arg)
{
    (void)i2c_slave;
    (void)evt_data;
    QueueHandle_t receive_queue = (QueueHandle_t)arg;
    i2c_slave_event_t evt = {
        .type = I2C_SLAVE_EVT_TX,
        .len = 0,
        .data = {0},
    };
    BaseType_t xTaskWoken = 0;
    xQueueSendFromISR(receive_queue, &evt, &xTaskWoken);
    return xTaskWoken;
}


/**
 * @brief 初始化外设（RGB灯、UART、I2C等）
 * @return 成功返回0，失败返回-1
 */
int peripheralInit(void)
{
    // 初始化RGB灯
    led_strip_install();
    int ret = led_strip_init(&strip);
    
    // 初始化GAT数据链表
    gatDataList = gatDataListInit();
    if (!gatDataList) {
        ESP_LOGE(TAG, "初始化GAT数据链表失败");
        return -1;
    }
    ESP_LOGI(TAG, "GAT数据链表初始化成功");
    uart_config_t config = {
        .baud_rate = GTA_UART_BAUDRATE,
        .data_bits = UART_DATA_BITS,
        .stop_bits = UART_STOP_BITS,
        .parity = UART_PARITY,
        .flow_ctrl = UART_FLOW_CTRL,
        .source_clk = UART_SOURCE_CLK,
    };
    // 初始化GTA UART
    uart_driver_install(GTA_UART_PORT, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0);
    uart_param_config(GTA_UART_PORT, &config);
    // 注意：uart_set_pin 之后不要再 gpio_set_pull_mode/gpio_config 同一脚，否则可能打断 UART 矩阵（此前只改 TX 会导致只收不发）
    ESP_ERROR_CHECK(uart_set_pin(GTA_UART_PORT, GTA_UART_TX_GPIO, GTA_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(gtaUartTask, "gtaUartTask", 8*1024, NULL, 10, NULL);
    xTaskCreate(userTask, "userTask", 8*1024, NULL, 10, NULL);
    // 配置GPIO为输入模式
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,      // 不使用中断
        .mode = GPIO_MODE_INPUT,             // 输入模式
        .pin_bit_mask = (1ULL << MODE_GPIO) | (1ULL << GAT_VDD_EN_GPIO) | (1ULL << GAT_RESET_GPIO) | (1ULL << GAT_SPI0_DO_GPIO | 1ULL << GAT_WAKEUP_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE     // 启动内部上拉（适合按键）
    };
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GAT_WAKEUP_INT_GPIO);
    gpio_config(&io_conf);
    gpio_set_level(GAT_WAKEUP_INT_GPIO, 1);
    snr9912UartInit();
    //gpio_set_level(GAT_WAKEUP_GPIO, 1); 


    {
        int mode_pin = gpio_get_level(MODE_GPIO);
#if COMM_MODE_FORCE >= 0
        commMode = COMM_MODE_FORCE;
#else
        commMode = mode_pin;
#endif
        ESP_LOGI(TAG, "MODE_GPIO(level=%d) COMM_MODE_FORCE=%d -> %s (SDA=%d SCL=%d addr=0x%02X)",
                 mode_pin, COMM_MODE_FORCE,
                 (commMode == I2C_MODE) ? "I2C_SLAVE" : "USER_UART",
                 USERUART_TX_SDA_GPIO, USERUART_RX_SCL_GPIO, USERI2C_ADDRESS);
    }
    if (commMode == I2C_MODE) {
        i2c_slave_config_t i2c_slv_config = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = I2C_SLAVE_NUM,
            /* 驱动环形缓冲：按块收发即可，整帧在应用层 i2cRxBuffer/i2cSlaveSendData */
            .send_buf_depth = 512,
            .receive_buf_depth = 512,
            .scl_io_num = USERUART_RX_SCL_GPIO,
            .sda_io_num = USERUART_TX_SDA_GPIO,
            .slave_addr = USERI2C_ADDRESS,
            .flags.enable_internal_pullup = true,
        };
        
        esp_err_t err = i2c_new_slave_device(&i2c_slv_config, &slave_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "创建I2C从机设备失败: %s", esp_err_to_name(err));
            return -1;
        }
        /* 队列项含 RX 拷贝，加深以防 sendText 分包连发 */
        slaveReceiveQueue = xQueueCreate(64, sizeof(i2c_slave_event_t));
        if (slaveReceiveQueue == NULL) {
            ESP_LOGE(TAG, "创建I2C队列失败");
            return -1;
        }
        i2c_slave_event_callbacks_t callbacks = {
            .on_receive = i2cSlaveReceiveDoneCb,
            .on_request = i2cSlaveRequestDoneCb,
        };
        ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(slave_handle, &callbacks, slaveReceiveQueue));
        ESP_LOGI(TAG, "I2C从机设备初始化成功，地址: 0x%02X", USERI2C_ADDRESS);
        xTaskCreate(i2cSlaveTask, "i2cSlaveTask", 8*1024, NULL, 15, NULL);
    } else {
        config.baud_rate = UART_BAUDRATE;
        uart_driver_install(USERUART_PORT, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0);
        uart_param_config(USERUART_PORT, &config);
        uart_set_pin(USERUART_PORT, USERUART_TX_SDA_GPIO, USERUART_RX_SCL_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        ESP_LOGI(TAG, "USERUART_PORT is initialized");
        xTaskCreate(userUartTask, "userUartTask", 8*1024, NULL, 5, NULL);
    }

#if GTA_HOST_CMD_SELFTEST
    /* 等 GTA 上报 0xA3 后再测；超时仍发送便于抓串口 */
    {
        const int wait_ms = 5000;
        int elapsed = 0;
        // while (!isOpen && elapsed < wait_ms) {
        //     vTaskDelay(pdMS_TO_TICKS(100));
        //     elapsed += 100;
        // }
        ESP_LOGI(TAG, "GTA selftest: isOpen=%d after %d ms", (int)isOpen, elapsed);

        /* DF A1 + 0x08 + FF */
        setGatVolume(0x06);
        vTaskDelay(pdMS_TO_TICKS(200));

        /* DF A2 + 0x08 + FF */
        setGatSpeed(0x03);
        vTaskDelay(pdMS_TO_TICKS(200));

        /* DF A0 + UTF-8「」+ FF */
        sendTextToGAT("欢迎使用");
        ESP_LOGI(TAG, "GTA selftest done (vol/speed/TTS)");
    }
#endif

    return ret;
}

typedef enum {
    RGB_FX_OFF = 0,
    RGB_FX_CYAN,
    RGB_FX_YELLOW_SLOW,
    RGB_FX_GREEN_TWICE,
    RGB_FX_RED_TWICE,
} rgb_fx_mode_t;

static volatile rgb_fx_mode_t rgbFxMode = RGB_FX_OFF;
static volatile uint32_t rgbFxSeq = 0;

static void rgbApplyCmd(uint8_t cmd)
{
    switch (cmd) {
    case 0x55:
        rgbFxMode = RGB_FX_CYAN;
        break;
    case 0x01:
        rgbFxMode = RGB_FX_YELLOW_SLOW;
        break;
    case 0x02:
        rgbFxMode = RGB_FX_GREEN_TWICE;
        break;
    case 0x03:
    case 0x04:
    case 0x05:
        rgbFxMode = RGB_FX_RED_TWICE;
        break;
    default:
        ESP_LOGW(TAG, "未知RGB命令: 0x%02X", cmd);
        return;
    }
    rgbFxSeq++;
    ESP_LOGI(TAG, "RGB命令: 0x%02X", cmd);
}

static void rgbFxTask(void *arg)
{
    uint32_t seen = 0;

    while (1) {
        rgb_fx_mode_t mode = rgbFxMode;
        uint32_t seq = rgbFxSeq;

        if (mode == RGB_FX_CYAN) {
            setRGBColor(RGB_CYAN);
            seen = seq;
            vTaskDelay(pdMS_TO_TICKS(50));
        } else if (mode == RGB_FX_YELLOW_SLOW) {
            seen = seq;
            setRGBColor(RGB_YELLOW);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (rgbFxSeq != seen) {
                continue;
            }
            setRGBColor(RGB_BLACK);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (mode == RGB_FX_GREEN_TWICE || mode == RGB_FX_RED_TWICE) {
            rgb_t color = (mode == RGB_FX_GREEN_TWICE) ? RGB_GREEN : RGB_RED;
            seen = seq;
            for (int i = 0; i < 2; i++) {
                if (rgbFxSeq != seen) {
                    break;
                }
                setRGBColor(color);
                vTaskDelay(pdMS_TO_TICKS(200));
                if (rgbFxSeq != seen) {
                    break;
                }
                setRGBColor(RGB_BLACK);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            while (rgbFxSeq == seen) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else {
            setRGBColor(RGB_BLACK);
            seen = seq;
            vTaskDelay(pdMS_TO_TICKS(50));
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
                    rgbApplyCmd(data);
                }
                state = (b == 0xF4) ? 1 : 0;
            }
        }
    }
}

static void snr9912UartInit(void)
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
    xTaskCreate(rgbFxTask, "rgbFxTask", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "SNR9912 UART GPIO%d/GPIO%d %d", SNR9912_UART_TX_GPIO, SNR9912_UART_RX_GPIO, SNR9912_UART_BAUDRATE);
}

/**
 * @brief 设置RGB灯颜色
 * @param color RGB颜色值
 * @return 成功返回0，失败返回非0
 */
int setRGBColor(rgb_t color)
{
    led_strip_set_pixel(&strip, 0, color);
    return led_strip_flush(&strip);
}

bool gatIsOpen(void)
{
    return isOpen;
}

void setGatVolume(uint8_t volume)
{
    if (volume > 0x0F) {
        volume = 0x0F;
    }
    /* DF A1 + vol(0x00~0x0F) + FF */
    sendBufToGAT(GAT_HOST_CMD_VOLUME, &volume, 1);
    ESP_LOGI(TAG, "setGatVolume: DF A1 %02X FF", volume);
}

void setGatSpeed(uint8_t speed)
{
    if (speed > 0x0F) {
        speed = 0x0F;
    }
    /* DF A2 + speed(0x00~0x0F) + FF */
    sendBufToGAT(GAT_HOST_CMD_SPEED, &speed, 1);
    ESP_LOGI(TAG, "setGatSpeed: DF A2 %02X FF", speed);
}

void sendTextToGAT(const char *text)
{
    if (!text || text[0] == '\0') {
        ESP_LOGW(TAG, "sendTextToGAT: empty text");
        return;
    }
    size_t len = strlen(text);
    if (len > VOREO_TTS_PAYLOAD_MAX) {
        ESP_LOGE(TAG, "sendTextToGAT: text too long (%u > %u)", (unsigned)len, VOREO_TTS_PAYLOAD_MAX);
        return;
    }
    /* DF A0 + UTF-8 + FF */
    sendBufToGAT(GAT_HOST_CMD_TTS, (const uint8_t *)text, len);
    ESP_LOGI(TAG, "sendTextToGAT: DF A0 + %u bytes UTF-8 + FF", (unsigned)len);
}


